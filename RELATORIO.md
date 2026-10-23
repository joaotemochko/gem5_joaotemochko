# IFA no MinorCPU (gem5 v25.1, RISC-V, SE): reordenação com backend em ordem

Branch `ifa-reorder`, a partir de `d054d05`. Nenhum commit foi feito. Todos os números vêm das simulações descritas em §8 (binário final, 304 execuções); os CSV completos estão em `ifa_eval/results/`.

## 1. Respostas objetivas

**Baseline × FEB sem reordenação × FEB com reordenação (backend MinorCPU inalterado, em ordem):**

| Benchmark | m0 (ciclos) | m1: FEB, despacho em ordem | m2: FEB + reordenação | **Ganho do buffer** (m1 vs m0) | **Ganho da reordenação** (m2 vs m1) |
|---|---:|---:|---:|---:|---:|
| Matmult | 3327979 | 3292840 | 3292848 | −1,056 % | +0,000 % (8 ciclos a mais) |
| CRC32 | 4225768 | 4225450 | 4225454 | −0,008 % | +0,000 % (4 ciclos a mais) |
| Wikisort | 2791157 | 2384706 | 2384096 | −14,562 % | **−0,026 %** |
| Blowfish | 41713249 | 39313260 | 39211138 | −5,754 % | **−0,260 %** |
| **28 programas** (média geom. de speedup) | 1 | 1,0992 | **1,1080** | +9,92 % | **+0,80 %** (melhor em 20, pior em 1, igual em 7) |

1. **A reordenação passou a ganhar.** Nos 28 programas ela melhora 20, empata 7 e perde 1 (`nettle-aes`, +0,03 %). Os maiores ganhos sobre o modo 1: `nettle-sha256` −7,26 %, `crc32-mib` −3,97 %, `sha` −2,13 %, `ud` −1,72 %, `depthconv` −1,54 %, `basicmath` −1,51 %, `md5sum` −1,33 %.
   - Na primeira versão (DU "mais velha pronta primeiro") ela **perdia** em 10 programas, inclusive no Blowfish (+0,56 %).
   - O que mudou foi o **critério de escolha da DU** (§3.1), com duas mudanças de suporte na FEB (§3.2).
   - O backend continua o mesmo MinorCPU em ordem. A única adição ao Execute é o PC de reinício de faltas, e só para o hoisting opcional (§3.3).
2. **Nos 4 benchmarks pedidos, o ganho da reordenação é pequeno ou nulo, por limitação dos próprios programas, não do IFA** (§4):
   - **CRC32** é uma cadeia de dependência serial com uma chamada (`jal rand_beebs`) por iteração. Não existe instrução independente para adiantar.
   - **Matmult:** o único trabalho independente da iteração (`addi` dos ponteiros) o `gcc -O2` já pôs antes do `mul`. Para ganhar seria preciso cruzar o `bne` do laço e subir o `ld` da próxima iteração acima do `sd` desta, o que exige desambiguação de endereço.
   - **Wikisort/Blowfish:** ganham pouco (−0,026 % e −0,260 %) porque já vêm bem escalonados do compilador.
   - A medição de por que o Execute para (nova stat `issueStopCycles`) confirma: em CRC32 e Matmult as paradas por operando não mudam com a reordenação (2 452 637 → 2 452 615; 1 316 943 → 1 316 856).
3. **O "ganho do buffer" (modo 1) é, na verdade, ganho de largura de decodificação e de busca antecipada.** O laço herdado do IFA original decodifica até uma linha inteira por ciclo para a FEB. Com decodificação realista de 2/ciclo (`ifa_decode_width=2`), o buffer sozinho dá só +0,68 % (média geom. 1,0068), e a reordenação acrescenta +0,38 % (1,0107), **cerca de um terço do ganho total do IFA** nessa configuração (melhor em 14 programas, pior em 1, `nettle-sha256` −7,04 %).
4. **Corretude:** 304 execuções no binário final (28 programas × 8 configurações + 4 benchmarks × 20 configurações). Saída do programa, código de saída e número de instruções commitadas são **idênticos ao baseline em todas**, inclusive com hoisting de loads/stores e faltas de pilha reais. O modo 0 é idêntico ao MinorCPU original ciclo a ciclo (28/28, e contra o binário do `d054d05` nos 4 benchmarks).
5. **`--enable-ifa` nunca ligou o IFA** no código de partida (§6.1). Resultados antigos obtidos com essa flag são do MinorCPU baseline.

## 2. Por que a primeira versão da reordenação não ganhava

Uma nova estatística de diagnóstico no Execute, `system.cpu.issueStopCycles::{Full,NoInput,Operands,FU,MemLimit,Other}`, conta, a cada ciclo, por que o issue parou. Com ela apareceram três fatos sobre o MinorCPU que a DU antiga ignorava:

1. **Um load/store encerra o ciclo de issue.** O laço de `Execute::issue` termina quando `num_mem_insts_issued == executeMemoryIssueLimit` (=1). O par "ALU, mem" sai num ciclo; "mem, ALU" gasta dois. No Blowfish (m0), 9,87 M dos 41,7 M ciclos param por `MemLimit`. O `gcc` não conhece essa regra.
2. **A latência que conta para o issue é a do scoreboard do Minor, por tipo de consumidor**: `returnCycle − srcRegsRelativeLats`. Um `add` depois de um load espera 1 ciclo; um `mul` depois de um load espera 3, porque o `mul` tem `srcRegsRelativeLats=[0]`. A DU antiga usava uma latência única por produtor.
3. **O commit é em ordem.** Tudo que vem depois de um load espera esse load no commit, então adiantar ALU sobre ALU só ajuda se reduzir paradas de issue. Mover uma instrução "pronta" à frente de uma "não pronta" pode **atrasar a cabeça**. Foi o que aconteceu no Blowfish: ALUs adiantadas criaram pares ruins com loads, e as paradas por `MemLimit` e `FU` subiram.

## 3. O que mudou na segunda rodada

### 3.1 DU por modelo de issue (`ifa_issue_model=True`, padrão do modo 2)

A DU passou a ser um **escalonador de lista** com um modelo do issue em ordem do Execute.

**Estimativa do ciclo de issue.** Para cada instrução da FEB que pode legalmente sair agora (regras de §5), `ifaEstimateIssue` calcula o ciclo em que ela emitiria se fosse a próxima despachada:
- não antes da última despachada (o issue é em ordem) nem antes de `agora + D`;
- para cada fonte, `returnCycle(produtor) − srcRegsRelativeLats(consumidor, fonte)`, exatamente como `Scoreboard::canInstIssue`. O `returnCycle` do produtor é o ciclo estimado de issue + `opLat + extraAssumedLat` da FU que o Execute usaria;
- FU não pipelinada ocupada (o divisor, `issueLat=9`);
- se o ciclo já tem `executeIssueLimit` instruções **ou já tem um load/store**, vai para o ciclo seguinte.

**Escolha.** Cada slot de saída recebe a instrução de **menor ciclo estimado**; no empate, a mais velha. Uma instrução mais nova só passa a cabeça se emitir **estritamente antes** dela, então, no modelo, a reordenação nunca atrasa a cabeça. Todas as latências vêm das descrições de FU do próprio Execute (`MinorFUPool`), sem constantes.

**Ablação (Tabela 4, Blowfish):**

| Configuração | vs m0 | vs m1 (mesmo fill) |
|---|---:|---:|
| "mais velha pronta", sem fill | −4,408 % | pior (+0,56 %) |
| modelo de issue, sem fill | −5,206 % | melhor (−0,28 %) |
| "mais velha pronta", com fill | −5,199 % | pior (+0,59 %) |
| modelo de issue, com fill (padrão) | **−5,998 %** | **melhor (−0,26 %)** |

### 3.2 FEB continua enchendo com o Decode cheio (`ifa_fill_when_blocked=True`, padrão nos modos 1 e 2)

- Antes, o Fetch2 só decodificava quando o Decode tinha espaço, então a FEB esvaziava justamente quando o Execute travava.
- Agora ele continua decodificando para a FEB (e prevendo desvios) enquanto o Decode está cheio, e só o despacho espera.
- Isso aumenta a janela da DU e também ajuda o modo 1. Blowfish: m1 −4,942 % → −5,754 %. Média geom. nos 28: 1,0976 → 1,0992.
- Só vale para `ifa_mode≠0`. Na rodada intermediária encontrei e corrigi um bug meu em que o padrão `True` vazava para o modo 0 e alterava o baseline; os resultados aqui são todos posteriores à correção, e o modo 0 bate ciclo a ciclo com o original.

### 3.3 Hoisting de loads/stores com reinício preciso (`ifa_hoist_memory_refs`, opcional, padrão False)

- Permite que uma referência à memória passe instruções mais velhas **sem memória** de que não depende. A ordem entre referências à memória é mantida.
- **Precisão sem ROB:** a referência só pode passar a cabeça da FEB se nada mais novo que a cabeça já tiver sido despachado. O IFA marca a instrução com `MinorDynInst::ifaRestartPC` = PC da cabeça.
- Se ela faltar, o Execute (`Execute::ifaPrepareFault`, chamado antes de `fault->invoke` nos três pontos de falta de memória) põe esse PC no thread. A falta então é tomada como se fosse na cabeça: em FS o EPC é o da cabeça, e em SE a execução recomeça dela.
- Como nada mais novo que a cabeça foi commitado antes, nada é executado duas vezes, e a **contagem de instruções commitadas fica idêntica**. Validado em 28 programas, com as faltas de crescimento de pilha do SE acontecendo.
- Ganho pequeno: média geom. 1,1082 contra 1,1080. Fica como opção.

### 3.4 Outros

- `ifa_queue_aware` (opcional): soma à estimativa de chegada a ocupação dos buffers de entrada do Decode e do Execute. Não ajudou com o modelo de issue (o modelo já acompanha a fila), por isso fica desligado.
- Instrumentação no Execute: `issueStopCycles`, ver §7.

## 4. Por que Matmult e CRC32 não ganham com reordenação em Fetch2

Laço interno do Matmult (`Multiply`, `-O2`):

```
102aa: ld   a4,0(a5)
102ac: ld   a0,0(a2)
102ae: addi a5,a5,8
102b0: addi a2,a2,160
102b4: mul  a4,a4,a0      # espera o ld (rel. lat. 0 no Mul FU) -> paradas "Operands"
102b8: add  a3,a3,a4
102ba: sd   a3,0(a6)
102be: bne  a5,a7,102aa
```

- As duas instruções independentes (`addi`) já estão entre os loads e o `mul`.
- Esconder a espera do `mul` exigiria trazer o `ld` da iteração seguinte, que está depois do `bne` (desvio é barreira; passar exigiria especulação com desfazer registradores) e depois do `sd` desta iteração (store→load sem endereço conhecido no Fetch2).
- Laço do CRC32 (`crc32pseudo`): `jal rand_beebs; xor; zext.b; slli; add; ld; srli; addiw; xor; bnez`. É uma cadeia serial pelo `ld` da tabela, com uma chamada por iteração.
- Nos dois casos, `issueStopCycles::Operands` não muda entre m1 e m2 (Tabela 1).

## 5. Corretude (regras de despacho e validação)

O Decode atribui o `execSeqNum` na ordem de chegada e o Execute emite e commita em ordem, então **a ordem de despacho do IFA é a ordem arquitetural de commit**. Regras (`Fetch2::ifaCanPass`, `ifaIsBarrier`):

| Aspecto | Regra |
|---|---|
| Registradores | candidato sem RAW/WAR/WAW com nenhuma instrução mais velha que ficou na FEB; `x0` (`InvalidRegClass`) ignorado |
| Desvios, `ecall`, CSR (`IsSerializeAfter/IsNonSpeculative`), `fence`, `fence.i`, AMO, `IsSquashAfter`, macro/micro-op, `IsQuiesce`, HTM, `IsInvalid`, falta de busca | barreira: não passa nada e nada a passa. Preserva `fetchSeqNum`/`predictionSeqNum` (histórico do preditor) e o descarte por `streamSeqNum` |
| Memória | por padrão, em ordem nos dois sentidos. Com hoisting: memória pode passar não-memória com PC de reinício (§3.3); memória nunca passa memória |
| FP/`fflags` | `fflags` só acumula e qualquer escrita de CSR é barreira, então FP entre FP é seguro |
| Commit fora de ordem de PC | `ExecContext` define o PC por instrução e `tryToBranch` compara o PC da própria instrução, sem desvio espúrio |
| Drain | `isDrained()` exige FEB vazia |

**Validação:**
- Programas: 19 Embench-IoT (todos com `verify=OK`), 8 execuções MiBench (`blowfish` e/d, `sha`, `crc32-mib`, `qsort`, `dijkstra`, `stringsearch`, `basicmath`) e um teste próprio `ifa_stress` (cadeias, aliasing load/store, AMO/LR-SC/`fence`, FP+`fflags`, ponteiros para função, recursão, `setjmp/longjmp`, syscalls).
- Critério: digest de stdout/stderr e arquivos escritos, código de saída e `commitStats0.numInsts` iguais aos do modo 0.
- Resultado: **304/304 idênticos** no binário final.
- A variante "ALU passa load reexecutável" (`ifa_pass_memory_refs`, vinda do RTL) dá estado correto, mas **+2 instruções commitadas** no Wikisort por replay após a falta de pilha; por isso fica desligada.

## 6. Problemas do código de partida (`d054d05`) e correções menores

1. **O despacho nunca reordenava.** O candidato era sempre `begin()` com `skipped_insts` vazia, então a cabeça sempre saía; era um FIFO.
2. **`--enable-ifa` não liga o IFA (item 3).** O `se.py` testava `hasattr(cpu, 'enableIFA')`, mas o parâmetro é `enable_ifa`; resultado: `enable_ifa=false` no `config.ini`. Verificado com o binário do `d054d05` (§6.1).
3. `ifa_latency`, `ifaLatencyCounter` e `isIFAStalled` não eram usados. Agora `ifa_latency` é a residência mínima na FEB (Tabela 7).
4. O `std::cout` foi removido; `cpu.cc`/`cpu.hh` voltaram a ser idênticos ao upstream.
5. **FEB não esvaziava em `isStreamChange()`.** Agora as entradas do fluxo antigo são descartadas (`ifaFlushes`, `ifaFlushedInsts`). O impacto é pequeno e misto (Tabela 7): sem flush, o Wikisort fica 0,16 pp mais rápido e o Blowfish 0,46 pp mais lento. Mantive o flush.
6. Outros problemas corrigidos:
   - falsas dependências por `x0` (`RegId()` == `RegId()`);
   - `isDrained()` ignorava a FEB;
   - FEB compartilhada entre threads (agora o IFA exige 1 thread);
   - `DPRINTF`s e comentários do upstream tinham sido removidos (o `fetch2.cc` foi refeito a partir do upstream).
7. **Build:** o fork perdeu o bit de execução dos 178 scripts executáveis do upstream. Com isso o `scons` falha, ou termina "done" sem gerar `gem5.opt` (o `libelf` falha em silêncio). Restaurei `+x` em 74 arquivos (`build_tools/`, `ext/`, `site_scons/`, `util/cpt_upgrader.py`), que aparecem como mudança de modo no `git status`. Há ainda 1 594 arquivos `*Zone.Identifier` versionados.

### 6.1 Verificação contra o código original

| Execução | Matmult | CRC32 | Wikisort | Blowfish |
|---|---:|---:|---:|---:|
| `d054d05`, `--enable-ifa` | 3327979 | 4225768 | 2791157 | 41713249 |
| esta branch, `--ifa-mode 0` | 3327979 | 4225768 | 2791157 | 41713249 |
| `d054d05`, IFA forçado com `-P system.cpu[0].enable_ifa=True` | 3292552 | 4225710 | 2380435 | 39675689 |

Uma variante temporária do modo 1 com as regras antigas (sem flush, parar o despacho após controle) reproduziu o IFA original forçado **ciclo a ciclo** em 4 programas, o que confirma que a reescrita da FEB é fiel.

## 7. Métrica de "stalls" (item 5)

- **Nenhuma estatística do gem5 vale 2×(ciclos − instruções).** O `stats.txt` tem quatro linhas iguais ao número de instruções commitadas (`simInsts`, `commitStats0.numInsts`, `thread_0.numInsts`, `executeStats0.numInsts`). O valor bate com `numCycles − numInsts` somado sobre duas delas, mas sem o script original não dá para dizer qual cálculo foi feito.
- Essa métrica fica **negativa** quando IPC > 1: no Blowfish m0, 2×(C−I) = −1 863 894.
- **Definição usada agora (medida, não inferida):** stalls = ciclos em que o Execute emitiu menos que `executeIssueLimit` instruções = `Σ issueStopCycles::{Operands, MemLimit, FU, NoInput, Other}`.
  - Cada componente diz a causa: operando não pronto, limite de 1 referência à memória por ciclo, FU ocupada/travada, nada para emitir (front-end).
  - Os ciclos em que o CPU fica ocioso (idle) não são avaliados.
- O CSV ainda traz `stall_slots = 2·C − I`, `stall_cycles = C − I/2` e a métrica antiga (`legacy_2x_c_minus_i`) para comparação.

## 8. Tabelas

Tempo = `simSeconds`; a 1 GHz, ciclos = ns. CSVs: `ifa_eval/results/phase5_4benchmarks.csv` (4 benchmarks × 20 configurações) e `ifa_eval/results/validation_all_workloads.csv` (28 programas × 8 configurações).

### Tabela 1 — Principal (FEB = 16, configuração padrão de cada modo)

| Benchmark | Modo | Tempo (ms) | Ciclos | Instr. | IPC | CPI | Stalls (ciclos)¹ | Branch misses | Issue: Operands | Issue: MemLimit | Issue: FU | Issue: NoInput | ifaDispatched | ifaDispatchedOutOfOrder | ifaHoisted | ifaSkipped | ifaHeadBlockedCycles | ifaReorderCycles | ifaBufferFullCycles | ifaFlushes | ifaFlushedInsts | Ocup. FEB |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Matmult | 0 | 3.328 | 3327979 | 2785718 | 0.837060 | 1.194658 | 2900611 | 17278 | 1316943 | 1061653 | 9956 | 512059 | — | — | — | — | — | — | — | — | — | — |
| Matmult | 1 | 3.293 | 3292840 | 2785718 | 0.845993 | 1.182044 | 2832155 | 17278 | 1316943 | 1027997 | 10151 | 477064 | 2922319 | 0 | 0 | 0 | 671529 | 0 | 45087 | 1165 | 11024 | 1.476 |
| Matmult | 2 | 3.293 | 3292848 | 2785718 | 0.845990 | 1.182046 | 2832075 | 17278 | 1316856 | 1027992 | 10159 | 477068 | 2922335 | 1001 | 0 | 5339 | 670661 | 0 | 45053 | 1164 | 11004 | 1.476 |
| CRC32 | 0 | 4.226 | 4225768 | 3857365 | 0.912820 | 1.095506 | 3511957 | 436 | 2452631 | 526535 | 905 | 531886 | — | — | — | — | — | — | — | — | — | — |
| CRC32 | 1 | 4.225 | 4225450 | 3857365 | 0.912889 | 1.095424 | 3512064 | 436 | 2452637 | 526507 | 1267 | 531653 | 3861361 | 0 | 0 | 0 | 378 | 0 | 1315 | 336 | 2003 | 0.802 |
| CRC32 | 2 | 4.225 | 4225454 | 3857365 | 0.912888 | 1.095425 | 3512041 | 436 | 2452615 | 526502 | 1272 | 531652 | 3861357 | 66 | 0 | 85 | 384 | 6 | 1313 | 336 | 2005 | 0.802 |
| Wikisort | 0 | 2.791 | 2791157 | 2077560 | 0.744336 | 1.343478 | 2226593 | 14763 | 189990 | 672875 | 209880 | 1153848 | — | — | — | — | — | — | — | — | — | — |
| Wikisort | 1 | 2.385 | 2384706 | 2077560 | 0.871202 | 1.147840 | 1802532 | 14734 | 249616 | 681764 | 244960 | 626192 | 2165224 | 0 | 0 | 0 | 36868 | 0 | 35266 | 5658 | 43718 | 1.700 |
| Wikisort | 2 | 2.384 | 2384096 | 2077560 | 0.871425 | 1.147546 | 1763430 | 14734 | 209113 | 682798 | 244393 | 627126 | 2157537 | 44770 | 0 | 49354 | 37748 | 362 | 35027 | 5652 | 51363 | 1.685 |
| Blowfish | 0 | 41.713 | 41713249 | 42645196 | 1.022342 | 0.978146 | 28604323 | 40882 | 8304927 | 9869875 | 2212358 | 8217163 | — | — | — | — | — | — | — | — | — | — |
| Blowfish | 1 | 39.313 | 39313260 | 42645196 | 1.084753 | 0.921868 | 26499475 | 40884 | 7992933 | 10173029 | 3062766 | 5270747 | 43408727 | 0 | 0 | 0 | 507813 | 0 | 16581237 | 31932 | 445096 | 9.389 |
| Blowfish | 2 | 39.211 | 39211138 | 42645196 | 1.087579 | 0.919474 | 26263171 | 40884 | 7677095 | 10134048 | 3180674 | 5271354 | 43408707 | 640712 | 0 | 2426532 | 507814 | 0 | 16439566 | 31930 | 445079 | 9.370 |

¹ Stalls = ciclos (avaliados) em que o Execute emitiu menos que `executeIssueLimit` instruções = soma de `issueStopCycles::{Operands,MemLimit,FU,NoInput,Other}`.

### Tabela 2 — Decomposição: buffer vs reordenação (ciclos)

| Benchmark | Ciclos m0 | Ciclos m1 | Ciclos m2 | Buffer (m1 vs m0) | Reordenação (m2 vs m1) | Total (m2 vs m0) |
|---|---:|---:|---:|---:|---:|---:|
| Matmult | 3327979 | 3292840 | 3292848 | -1.056% | +0.000% | -1.056% |
| CRC32 | 4225768 | 4225450 | 4225454 | -0.008% | +0.000% | -0.007% |
| Wikisort | 2791157 | 2384706 | 2384096 | -14.562% | -0.026% | -14.584% |
| Blowfish | 41713249 | 39313260 | 39211138 | -5.754% | -0.260% | -5.998% |

### Tabela 3 — Os 28 programas (ciclos, % vs modo 0)

| Programa | m1 (buffer) | m2 (buffer + reordenação) | m2 vs m1 | m1, decod. 2/ciclo | m2, decod. 2/ciclo | m2 vs m1 (2/ciclo) |
|---|---:|---:|---:|---:|---:|---:|
| aha-mont64 | -19.07% | -19.07% | -0.00% | -0.27% | -0.27% | -0.00% |
| basicmath | -9.66% | -11.03% | -1.51% | -0.70% | -0.88% | -0.19% |
| blowfish | -5.75% | -6.00% | -0.26% | -0.32% | -0.58% | -0.27% |
| blowfish-d | -7.94% | -8.13% | -0.20% | -1.84% | -2.12% | -0.29% |
| crc32 | -0.01% | -0.01% | +0.00% | -0.01% | -0.01% | -0.00% |
| crc32-mib | -13.77% | -17.19% | -3.97% | +0.00% | -0.00% | -0.00% |
| depthconv | -0.12% | -1.66% | -1.54% | -0.11% | -1.36% | -1.25% |
| dijkstra | -13.76% | -13.90% | -0.17% | -0.22% | -0.22% | -0.00% |
| edn | -24.81% | -24.86% | -0.06% | -0.06% | -0.13% | -0.07% |
| huffbench | -6.92% | -6.93% | -0.02% | -0.04% | -0.06% | -0.02% |
| ifa_stress | -7.74% | -8.10% | -0.39% | -0.01% | -0.39% | -0.38% |
| matmult-int | -1.06% | -1.06% | +0.00% | +0.00% | +0.01% | +0.00% |
| md5sum | -13.44% | -14.59% | -1.33% | -0.03% | -0.03% | +0.00% |
| nettle-aes | -0.68% | -0.66% | +0.03% | -0.60% | -0.55% | +0.05% |
| nettle-sha256 | -0.68% | -7.89% | -7.26% | -0.55% | -7.55% | -7.04% |
| nsichneu | -5.57% | -5.57% | -0.00% | -0.11% | -0.11% | +0.00% |
| picojpeg | -11.08% | -11.59% | -0.57% | -0.64% | -0.66% | -0.02% |
| qrduino | -4.80% | -5.24% | -0.46% | -0.04% | -0.19% | -0.15% |
| qsort | -10.34% | -10.43% | -0.09% | -2.85% | -2.87% | -0.02% |
| sglib-combined | -6.14% | -6.17% | -0.03% | -0.10% | -0.10% | -0.00% |
| sha | -16.30% | -18.08% | -2.13% | -0.00% | -0.00% | +0.00% |
| slre | -14.28% | -14.31% | -0.03% | -2.79% | -2.80% | -0.01% |
| statemate | -5.70% | -5.70% | -0.00% | -5.39% | -5.39% | -0.00% |
| stringsearch | -6.72% | -6.76% | -0.05% | -0.14% | -0.18% | -0.04% |
| tarfind | -8.25% | -8.25% | -0.00% | +0.12% | +0.12% | +0.00% |
| ud | -10.51% | -12.05% | -1.72% | -1.89% | -2.62% | -0.75% |
| wikisort | -14.56% | -14.58% | -0.03% | -0.15% | -0.16% | -0.01% |
| xgboost | -7.59% | -7.59% | -0.00% | -0.01% | -0.01% | -0.00% |
| **média geom. speedup** | 1.0992 | 1.1080 | ×1.0080 | 1.0068 | 1.0107 | ×1.0038 |

m2 vs m1 (decodificação sem limite): melhor em 20, pior em 1, igual (|Δ| ≤ 0,01 %) em 7. Com decodificação 2/ciclo: melhor em 14, pior em 1, igual em 13.

### Tabela 4 — Ablação dos mecanismos do modo 2 (ciclos; % vs modo 0)

| Configuração | Matmult | CRC32 | Wikisort | Blowfish |
|---|---:|---:|---:|---:|
| modo 1, sem fill | 3292861 (-1.055%) | 4225673 (-0.002%) | 2385682 (-14.527%) | 39651852 (-4.942%) |
| modo 1 (padrão: fill) | 3292840 (-1.056%) | 4225450 (-0.008%) | 2384706 (-14.562%) | 39313260 (-5.754%) |
| modo 2, escalonador "mais velha pronta", sem fill (DU da 1ª rodada) | 3292861 (-1.055%) | 4225673 (-0.002%) | 2385456 (-14.535%) | 39874711 (-4.408%) |
| modo 2, "mais velha pronta", com fill | 3292847 (-1.056%) | 4225450 (-0.008%) | 2384364 (-14.574%) | 39544582 (-5.199%) |
| modo 2, modelo de issue, sem fill | 3293152 (-1.046%) | 4225677 (-0.002%) | 2384887 (-14.556%) | 39541535 (-5.206%) |
| modo 2 (padrão: modelo de issue + fill) | 3292848 (-1.056%) | 4225454 (-0.007%) | 2384096 (-14.584%) | 39211138 (-5.998%) |
| modo 2 + hoisting de memória | 3292848 (-1.056%) | 4225454 (-0.007%) | 2384096 (-14.584%) | 39250564 (-5.904%) |

### Tabela 5 — Tamanho da FEB (ciclos; % vs modo 0)

| Configuração | Matmult | CRC32 | Wikisort | Blowfish |
|---|---:|---:|---:|---:|
| modo 1, FEB 8 | 3294330 (-1.011%) | 4225668 (-0.002%) | 2388506 (-14.426%) | 39666120 (-4.908%) |
| modo 2, FEB 8 | 3294330 (-1.011%) | 4225665 (-0.002%) | 2388284 (-14.434%) | 39588134 (-5.095%) |
| modo 1, FEB 16 | 3292840 (-1.056%) | 4225450 (-0.008%) | 2384706 (-14.562%) | 39313260 (-5.754%) |
| modo 2, FEB 16 | 3292848 (-1.056%) | 4225454 (-0.007%) | 2384096 (-14.584%) | 39211138 (-5.998%) |
| modo 1, FEB 32 | 3292795 (-1.057%) | 4225353 (-0.010%) | 2382564 (-14.639%) | 39502120 (-5.301%) |
| modo 2, FEB 32 | 3292777 (-1.058%) | 4225353 (-0.010%) | 2381472 (-14.678%) | 39391660 (-5.566%) |

### Tabela 6 — Largura de decodificação para a FEB (ciclos; % vs modo 0)

| Configuração | Matmult | CRC32 | Wikisort | Blowfish |
|---|---:|---:|---:|---:|
| modo 1, 2/ciclo | 3328124 (+0.004%) | 4225407 (-0.009%) | 2786893 (-0.153%) | 41579864 (-0.320%) |
| modo 2, 2/ciclo | 3328185 (+0.006%) | 4225405 (-0.009%) | 2786600 (-0.163%) | 41469253 (-0.585%) |
| modo 1, 4/ciclo | 3293228 (-1.044%) | 4225517 (-0.006%) | 2384728 (-14.561%) | 39602914 (-5.059%) |
| modo 2, 4/ciclo | 3293228 (-1.044%) | 4225521 (-0.006%) | 2384029 (-14.586%) | 39500666 (-5.304%) |
| modo 1, sem limite (padrão) | 3292840 (-1.056%) | 4225450 (-0.008%) | 2384706 (-14.562%) | 39313260 (-5.754%) |
| modo 2, sem limite (padrão) | 3292848 (-1.056%) | 4225454 (-0.007%) | 2384096 (-14.584%) | 39211138 (-5.998%) |

### Tabela 7 — Flush em mudança de fluxo e ifa_latency (ciclos; % vs modo 0)

| Configuração | Matmult | CRC32 | Wikisort | Blowfish |
|---|---:|---:|---:|---:|
| modo 1, flush (padrão) | 3292840 (-1.056%) | 4225450 (-0.008%) | 2384706 (-14.562%) | 39313260 (-5.754%) |
| modo 1, sem flush | 3292406 (-1.069%) | 4225472 (-0.007%) | 2380235 (-14.722%) | 39504833 (-5.294%) |
| modo 2, flush (padrão) | 3292848 (-1.056%) | 4225454 (-0.007%) | 2384096 (-14.584%) | 39211138 (-5.998%) |
| modo 2, sem flush | 3292351 (-1.071%) | 4225471 (-0.007%) | 2379496 (-14.749%) | 39417560 (-5.504%) |
| modo 1, ifa_latency=1 | 3277138 (-1.528%) | 4225719 (-0.001%) | 2415440 (-13.461%) | 39379445 (-5.595%) |
| modo 2, ifa_latency=1 | 3277203 (-1.526%) | 4225724 (-0.001%) | 2415263 (-13.467%) | 39316101 (-5.747%) |

### Tabela 8 — Média geométrica de speedup vs modo 0, 28 programas

| Config. | m1 | m2 | m1 2/ciclo | m2 2/ciclo | m2 FEB 8 | m2 FEB 32 | m2 + hoist |
|---|---:|---:|---:|---:|---:|---:|---:|
| speedup | 1.0992 | 1.1080 | 1.0068 | 1.0107 | 1.0923 | 1.1114 | 1.1082 |

## 9. Parâmetros

| Parâmetro (`BaseMinorCPU`) | `se.py` | Padrão | Significado |
|---|---|---|---|
| `ifa_mode` | `--ifa-mode` | 0 | 0 = MinorCPU original; 1 = FEB, despacho em ordem; 2 = FEB + reordenação |
| `ifa_buffer_size` | `--ifa-buffer-size` | 16 | entradas da FEB |
| `ifa_decode_width` | `--ifa-decode-width` | 0 | instruções decodificadas para a FEB por ciclo (0 = sem limite, como no código original) |
| `ifa_fill_when_blocked` | `--[no-]ifa-fill-when-blocked` | True | FEB continua enchendo com o Decode cheio (só com `ifa_mode≠0`) |
| `ifa_issue_model` | `--[no-]ifa-issue-model` | True | DU por modelo de issue (False = "mais velha pronta primeiro", a DU da 1ª rodada) |
| `ifa_hoist_memory_refs` | `--ifa-hoist-memory-refs` | False | loads/stores passam não-memória, com reinício preciso |
| `ifa_latency` | `--ifa-latency` | 0 | residência mínima na FEB |
| `ifa_flush_on_stream_change` | `--ifa-no-flush` | True | descarta o fluxo antigo da FEB |
| `ifa_queue_aware` | `--ifa-queue-aware` | False | estimativa de chegada inclui a fila Decode/Execute |
| `ifa_dau_source`, `ifa_load_latency` | `--ifa-dau-source`, `--ifa-load-latency` | IFAHybrid, 4 | só para a DU "mais velha pronta" (shadow scoreboard × scoreboard do Execute com cópia registrada no início do ciclo, ou seja, defasagem de 1 ciclo) |
| `ifa_pass_memory_refs` | `--ifa-pass-memory-refs` | False | ALU reexecutável passa memória (muda a contagem de instruções em faltas) |

Stats novas: `system.cpu.fetch2.ifa{Dispatched, DispatchedOutOfOrder, Hoisted, Skipped, HeadBlockedCycles, ReorderCycles, BufferFullCycles, Flushes, FlushedInsts, Occupancy}` e `system.cpu.issueStopCycles::*`. No modo `issue_model`:
- `ifaSkipped` = total de instruções mais velhas ultrapassadas pelos despachos fora de ordem;
- `ifaHeadBlockedCycles` = ciclos em que o modelo estima que a cabeça emitiria mais de 1 ciclo depois do mais cedo possível.

## 10. Configuração e reprodução

**Configuração efetiva** (lida do `config.ini`):
- `RiscvMinorCPU` RV64GC a 1 GHz.
- `decodeInputWidth=2`, `executeInputWidth=2`, `executeIssueLimit=2`, `executeMemoryIssueLimit=1`, `executeCommitLimit=2`, `executeMemoryCommitLimit=1`.
- Buffers: Fetch2 2 linhas, Decode 3 e Execute 7 pacotes.
- Preditor: `TournamentBP` (local 2048, global 8192, choice 8192).
- L1-I 32 KiB e L1-D 64 KiB, 2 vias (tag 2 + data 2 + response 2), sem L2.
- `DDR3_1600_8x8`. **Latência média de miss medida: 55–57 ciclos**, não ~100.

**Compilação cruzada:** `riscv64-unknown-elf-gcc` 16.1.0, `-O2 -std=gnu17 -march=rv64gc -mabi=lp64d -static`.
- Embench com `GLOBAL_SCALE_FACTOR=1`.
- MiBench `small`, com um shim `_open`→`openat` (o libgloss usa a syscall 1024 do proxy kernel).
- As entradas ficam fora de `/tmp` (o gem5 SE redireciona o `/tmp` do convidado).

```sh
scons build/RISCV/gem5.opt -j$(nproc)                  # SCons 4.7 num venv
ifa_eval/build_benchmarks.sh <embench-iot> <mibench> <bench>
python3 ifa_eval/run.py --gem5 build/RISCV/gem5.opt --bench <bench> --mibench <mibench> \
    --out <runs> "m0=--ifa-mode 0" "m1=--ifa-mode 1" "m2=--ifa-mode 2" \
    "m1-dw2=--ifa-mode 1 --ifa-decode-width 2" "m2-dw2=--ifa-mode 2 --ifa-decode-width 2"
python3 ifa_eval/collect.py <runs> out.csv --baseline m0  # CSV + checagem de saída/contagem
```

Execuções em `/home/ubuntu/ifa_bench/runs/final_{val,exp}`; binários em `/home/ubuntu/ifa_bench/bin` (`gem5_final.opt` = esta branch, `gem5_orig.opt` = `d054d05`).

## 11. Limitações

- Só SE, 1 thread, RISC-V. Drain/checkpoint não foram exercitados.
- **Interrupções (FS):** se uma interrupção cair entre instruções reordenadas, o PC de retorno seria o da próxima a commitar, e não o da mais velha pendente. Em SE não há interrupções. Em FS seria preciso adiar a interrupção até a cabeça da FEB alcançar o commit, ou usar o mesmo PC de reinício do hoisting.
- O modelo de issue ignora as paradas de commit (FU travada esperando o load no fim do pipeline) e as `timings` por máscara de opcode (usa `timings[0]`).
- `ifa_decode_width=0` (padrão herdado) supõe decodificar uma linha inteira por ciclo, um custo de hardware não modelado. Com 2/ciclo, o ganho total do IFA é 1,07 % (média geom.), dos quais 0,38 pontos vêm da reordenação.
- Os ganhos sobre código `-O2` são limitados porque o compilador já escalona. O potencial da reordenação aparece onde o escalonamento estático não conhece o Minor (`nettle-sha256`, `crc32-mib`, `sha`, `ud`).

## Apêndice A — Diff contra o gem5 v25.1 upstream

`diff -u` contra a tag `v25.1`; os cabeçalhos `@@ -a,b +c,d @@` dão as linhas no upstream e **neste** arquivo. O mesmo conteúdo está em `ifa_eval/diff_vs_upstream_v25.1.patch`; o diff contra o `d054d05` está em `ifa_eval/diff_vs_d054d05.patch`.

```diff
--- a/src/cpu/minor/BaseMinorCPU.py
+++ b/src/cpu/minor/BaseMinorCPU.py
@@ -298,11 +298,75 @@
     vals = ["SingleThreaded", "RoundRobin", "Random"]
 
 
+class MinorIFADAUSource(Enum):
+    """Where the IFA's dependency analysis unit (DAU) looks for producers
+    which are still in flight: the shadow scoreboard kept in Fetch2,
+    Execute's scoreboard (as it was at the start of the cycle), or both"""
+
+    vals = ["IFAShadow", "IFAScoreboard", "IFAHybrid"]
+
+
 class BaseMinorCPU(BaseCPU):
     type = "BaseMinorCPU"
     cxx_header = "cpu/minor/cpu.hh"
     cxx_class = "gem5::MinorCPU"
 
+    ifa_mode = Param.Unsigned(
+        0,
+        "Instruction Flow Amplifier (IFA) in Fetch2: 0 = off (baseline"
+        " MinorCPU), 1 = front-end buffer (FEB) with in-order dispatch,"
+        " 2 = FEB with out-of-order dispatch of the instructions the"
+        " dependency analysis unit finds ready",
+    )
+    ifa_buffer_size = Param.Unsigned(16, "Number of IFA FEB entries")
+    ifa_decode_width = Param.Unsigned(
+        0,
+        "Maximum number of instructions decoded into the IFA FEB each cycle"
+        " (0 means no limit other than FEB space and available input)",
+    )
+    ifa_latency = Param.Cycles(
+        0,
+        "Cycles an instruction spends in the IFA FEB before it can be"
+        " dispatched (0 means in the cycle it is decoded)",
+    )
+    ifa_flush_on_stream_change = Param.Bool(
+        True,
+        "Discard IFA FEB entries from the old stream when Execute changes"
+        " stream",
+    )
+    ifa_dau_source = Param.MinorIFADAUSource(
+        "IFAHybrid",
+        "Where the IFA DAU finds in-flight producers",
+    )
+    ifa_load_latency = Param.Cycles(
+        4, "Load-to-use latency assumed by the IFA shadow scoreboard"
+    )
+    ifa_issue_model = Param.Bool(
+        True,
+        "Choose the IFA dispatch order with a model of Execute's in-order"
+        " issue (the instruction which would issue earliest goes first)"
+        " instead of oldest-ready-first",
+    )
+    ifa_hoist_memory_refs = Param.Bool(
+        False,
+        "Let the IFA dispatch memory references before older instructions"
+        " they don't depend on (faults restart from the oldest of them)",
+    )
+    ifa_fill_when_blocked = Param.Bool(
+        True, "Keep decoding into the IFA FEB while Decode is full"
+    )
+    ifa_queue_aware = Param.Bool(
+        False,
+        "Count the instructions queued in Decode and Execute when the IFA"
+        " DAU estimates when an instruction will reach issue",
+    )
+    ifa_pass_memory_refs = Param.Bool(
+        False,
+        "Let the IFA dispatch instructions which can be safely executed"
+        " again before older memory references.  If the memory reference"
+        " faults, they are executed (and committed) again",
+    )
+
     @classmethod
     def memory_mode(cls):
         return "timing"
--- a/src/cpu/minor/SConscript
+++ b/src/cpu/minor/SConscript
@@ -44,7 +44,7 @@
     SimObject('BaseMinorCPU.py', sim_objects=[
         'MinorOpClass', 'MinorOpClassSet', 'MinorFUTiming', 'MinorFU',
         'MinorFUPool', 'BaseMinorCPU'],
-        enums=['ThreadPolicy'])
+        enums=['ThreadPolicy', 'MinorIFADAUSource'])
 
     Source('activity.cc')
     Source('cpu.cc')
--- a/src/cpu/minor/dyn_inst.hh
+++ b/src/cpu/minor/dyn_inst.hh
@@ -191,6 +191,12 @@
     /** Predicted branch target */
     std::unique_ptr<PCStateBase> predictedTarget;
 
+    /** Set by the IFA in Fetch2 on a memory reference which it dispatched
+     *  before older instructions.  If the reference faults, execution
+     *  restarts from this PC (the oldest of those instructions) so that the
+     *  fault is precise */
+    std::unique_ptr<PCStateBase> ifaRestartPC;
+
     /** Fields only set during execution */
 
     /** FU this instruction is issued to */
--- a/src/cpu/minor/fetch2.hh
+++ b/src/cpu/minor/fetch2.hh
@@ -45,13 +45,17 @@
 #ifndef __CPU_MINOR_FETCH2_HH__
 #define __CPU_MINOR_FETCH2_HH__
 
+#include <deque>
 #include <vector>
 
 #include "base/named.hh"
 #include "cpu/minor/buffers.hh"
 #include "cpu/minor/cpu.hh"
+#include "cpu/minor/execute.hh"
 #include "cpu/minor/pipe_data.hh"
+#include "cpu/minor/scoreboard.hh"
 #include "cpu/pred/bpred_unit.hh"
+#include "enums/MinorIFADAUSource.hh"
 #include "params/BaseMinorCPU.hh"
 
 namespace gem5
@@ -94,6 +98,126 @@
     /** Branch predictor passed from Python configuration */
     branch_prediction::BPredUnit &branchPredictor;
 
+    /** Instruction Flow Amplifier (IFA) mode.  0: baseline Fetch2 (IFA
+     *  off).  1: decoded instructions go through the front-end buffer
+     *  (FEB) and leave it in program order.  2: the dispatch unit (DU)
+     *  sends the oldest instructions the dependency analysis unit (DAU)
+     *  finds ready, which may skip a blocked head */
+    const unsigned int ifaMode;
+
+    /** Number of FEB entries */
+    const unsigned int ifaBufferSize;
+
+    /** Maximum number of instructions decoded into the FEB each cycle.
+     *  0 means no limit other than FEB space and available input */
+    const unsigned int ifaDecodeWidth;
+
+    /** Cycles an instruction spends in the FEB before it can be
+     *  dispatched.  0 allows dispatch in the cycle it is decoded */
+    const Cycles ifaLatency;
+
+    /** Discard FEB entries from the old stream when Execute changes
+     *  stream.  If false, they are dispatched and dropped by Execute */
+    const bool ifaFlushOnStreamChange;
+
+    /** Where the DAU finds producers which are still in flight */
+    const enums::MinorIFADAUSource ifaDAUSource;
+
+    /** Load-to-use latency assumed by the shadow scoreboard */
+    const Cycles ifaLoadLatency;
+
+    /** Allow instructions which can be executed again with the same result
+     *  to pass older memory references.  If such a memory reference
+     *  faults (for example, a stack page fault in SE mode), the passing
+     *  instructions are executed and committed again after the fault */
+    const bool ifaPassMemoryRefs;
+
+    /** Allow memory references to be dispatched before older (non memory)
+     *  instructions they don't depend on, so that they start earlier.
+     *  Only the first instruction to pass the head may be a memory
+     *  reference, and if it faults, Execute restarts from the head
+     *  (MinorDynInst::ifaRestartPC), keeping faults precise */
+    const bool ifaHoistMemoryRefs;
+
+    /** Keep decoding into the FEB while Decode can't accept instructions */
+    const bool ifaFillWhenBlocked;
+
+    /** Include the instructions queued between Fetch2 and issue in the
+     *  DAU's estimate of when an instruction will reach issue */
+    const bool ifaQueueAware;
+
+    /** Capacity of Decode's input buffer */
+    const unsigned int decodeInputBufferSize;
+
+    /** Choose what to dispatch with a model of Execute's in-order issue:
+     *  the instruction which would issue earliest goes first, so a younger
+     *  instruction only passes the head if it would issue before it.  If
+     *  false, the oldest ready instruction goes first */
+    const bool ifaIssueModel;
+
+    /** Execute's executeIssueLimit */
+    const unsigned int ifaIssueLimit;
+
+    /** Issue model: by OpClass, as Execute's FUs see it: the latency
+     *  added to the issue cycle to give the scoreboard's returnCycle, the
+     *  source relative latencies, the FU used and its issue latency */
+    std::vector<Cycles> ifaFUOpLat;
+    std::vector<std::vector<Cycles>> ifaFURelLats;
+    std::vector<int> ifaOpFU;
+    std::vector<Cycles> ifaFUIssueLat;
+
+    /** Issue model state: the estimated issue cycle of the last dispatched
+     *  instruction, the number issued in that cycle, whether one of them
+     *  was a memory reference (which ends Execute's issue for the cycle)
+     *  and, by FU, the first cycle it can accept an instruction */
+    Cycles ifaModelLastIssue;
+    unsigned int ifaModelNumAtLast;
+    bool ifaModelMemAtLast;
+    std::vector<Cycles> ifaModelFUFree;
+
+    /** Execute, whose scoreboard the DAU reads */
+    const Execute &execute;
+
+    /** Earliest number of cycles between an instruction leaving Fetch2
+     *  and it reaching issue in Execute */
+    const Cycles ifaDispatchToIssue;
+
+    /** An FEB entry */
+    struct IFAEntry
+    {
+        MinorDynInstPtr inst;
+
+        /** Cycle at which the entry can first be dispatched */
+        Cycles dispatchableCycle;
+    };
+
+    /** The FEB, oldest instruction first.  Entries leave it when they are
+     *  dispatched, so the front is always the oldest undispatched
+     *  instruction (the head) */
+    std::deque<IFAEntry> ifaBuffer;
+
+    /** Shadow scoreboard.  Only its register indexing and returnCycle are
+     *  used: returnCycle holds, for each register, the estimated cycle
+     *  from which an instruction can issue in Execute after reading the
+     *  value of the last dispatched instruction writing that register */
+    Scoreboard ifaShadow;
+
+    /** Estimated cycles between a producer and a consumer issuing, by the
+     *  producer's OpClass.  Taken from the Execute FU descriptions,
+     *  except for memory operations, which use ifaLoadLatency */
+    std::vector<Cycles> ifaOpLatency;
+
+    /** Relative latency at which an integer consumer reads its sources,
+     *  used when comparing against Execute's own returnCycle */
+    Cycles ifaSrcRelativeLatency;
+
+    /** Estimated cycle at which instructions dispatched in this cycle reach
+     *  issue in Execute */
+    Cycles ifaArrival;
+
+    /** Youngest (largest fetchSeqNum) instruction dispatched so far */
+    InstSeqNum ifaMaxDispatchedSeqNum;
+
   public:
     /* Public so that Pipeline can pass it to Fetch1 */
     std::vector<InputBuffer<ForwardLineData>> inputBuffer;
@@ -162,7 +286,7 @@
 
     struct Fetch2Stats : public statistics::Group
     {
-        Fetch2Stats(MinorCPU *cpu);
+        Fetch2Stats(MinorCPU *cpu, unsigned int ifa_buffer_size);
         /** Stats */
         statistics::Scalar totalInstructions;
         statistics::Scalar intInstructions;
@@ -171,6 +295,18 @@
         statistics::Scalar loadInstructions;
         statistics::Scalar storeInstructions;
         statistics::Scalar amoInstructions;
+
+        /** IFA stats */
+        statistics::Scalar ifaDispatched;
+        statistics::Scalar ifaDispatchedOutOfOrder;
+        statistics::Scalar ifaHoisted;
+        statistics::Scalar ifaSkipped;
+        statistics::Scalar ifaHeadBlockedCycles;
+        statistics::Scalar ifaReorderCycles;
+        statistics::Scalar ifaBufferFullCycles;
+        statistics::Scalar ifaFlushes;
+        statistics::Scalar ifaFlushedInsts;
+        statistics::Distribution ifaOccupancy;
     } stats;
 
   protected:
@@ -198,10 +334,39 @@
      *  fetch from. */
     ThreadID getScheduledThread();
 
+    /** IFA: does this instruction have to leave the FEB in program order?
+     *  Nothing younger may pass it and it may not pass anything older */
+    bool ifaIsBarrier(const MinorDynInstPtr &inst) const;
+
+    /** IFA: DAU test that all the sources of inst will be available when
+     *  it reaches issue, if it is dispatched now */
+    bool ifaSourcesReady(const MinorDynInstPtr &inst);
+
+    /** IFA: DAU test that inst can be dispatched before all the (older)
+     *  instructions in skipped, which are still in the FEB */
+    bool ifaCanPass(const MinorDynInstPtr &inst,
+        const std::vector<MinorDynInstPtr> &skipped) const;
+
+    /** IFA: note the destinations of a dispatched instruction in the shadow
+     *  scoreboard */
+    void ifaMarkupDispatch(const MinorDynInstPtr &inst);
+
+    /** IFA: dispatch instructions from the FEB into insts_out */
+    void ifaDispatch(ForwardInstData &insts_out);
+
+    /** IFA issue model: the cycle in which inst would issue in Execute if
+     *  it were dispatched next */
+    Cycles ifaEstimateIssue(const MinorDynInstPtr &inst) const;
+
+    /** IFA issue model: account for inst being dispatched to issue at
+     *  issue_cycle */
+    void ifaModelDispatch(const MinorDynInstPtr &inst, Cycles issue_cycle);
+
   public:
     Fetch2(const std::string &name,
         MinorCPU &cpu_,
         const BaseMinorCPUParams &params,
+        const Execute &execute_,
         Latch<ForwardLineData>::Output inp_,
         Latch<BranchData>::Output branchInp_,
         Latch<BranchData>::Input predictionOut_,
--- a/src/cpu/minor/fetch2.cc
+++ b/src/cpu/minor/fetch2.cc
@@ -37,6 +37,7 @@
 
 #include "cpu/minor/fetch2.hh"
 
+#include <algorithm>
 #include <string>
 
 #include "arch/generic/decoder.hh"
@@ -58,6 +59,7 @@
 Fetch2::Fetch2(const std::string &name,
     MinorCPU &cpu_,
     const BaseMinorCPUParams &params,
+    const Execute &execute_,
     Latch<ForwardLineData>::Output inp_,
     Latch<BranchData>::Output branchInp_,
     Latch<BranchData>::Input predictionOut_,
@@ -73,8 +75,39 @@
     outputWidth(params.decodeInputWidth),
     processMoreThanOneInput(params.fetch2CycleInput),
     branchPredictor(*params.branchPred),
+    ifaMode(params.ifa_mode),
+    ifaBufferSize(params.ifa_buffer_size),
+    ifaDecodeWidth(params.ifa_decode_width),
+    ifaLatency(params.ifa_latency),
+    ifaFlushOnStreamChange(params.ifa_flush_on_stream_change),
+    ifaDAUSource(params.ifa_dau_source),
+    ifaLoadLatency(params.ifa_load_latency),
+    ifaPassMemoryRefs(params.ifa_pass_memory_refs),
+    ifaHoistMemoryRefs(params.ifa_hoist_memory_refs),
+    ifaFillWhenBlocked(params.ifa_mode != 0 && params.ifa_fill_when_blocked),
+    ifaQueueAware(params.ifa_queue_aware),
+    decodeInputBufferSize(params.decodeInputBufferSize),
+    ifaIssueModel(params.ifa_issue_model),
+    ifaIssueLimit(params.executeIssueLimit),
+    ifaFUOpLat(Num_OpClasses, Cycles(0)),
+    ifaFURelLats(Num_OpClasses),
+    ifaOpFU(Num_OpClasses, -1),
+    ifaFUIssueLat(Num_OpClasses, Cycles(1)),
+    ifaModelLastIssue(0),
+    ifaModelNumAtLast(0),
+    ifaModelMemAtLast(false),
+    ifaModelFUFree(params.executeFuncUnits->funcUnits.size(), Cycles(0)),
+    execute(execute_),
+    ifaDispatchToIssue(params.fetch2ToDecodeForwardDelay +
+        params.decodeToExecuteForwardDelay),
+    ifaShadow(name + ".ifaShadow",
+        cpu_.threads[0]->getIsaPtr()->regClasses()),
+    ifaOpLatency(Num_OpClasses, Cycles(0)),
+    ifaSrcRelativeLatency(0),
+    ifaArrival(0),
+    ifaMaxDispatchedSeqNum(0),
     fetchInfo(params.numThreads),
-    threadPriority(0), stats(&cpu_)
+    threadPriority(0), stats(&cpu_, params.ifa_buffer_size)
 {
     if (outputWidth < 1)
         fatal("%s: decodeInputWidth must be >= 1 (%d)\n", name, outputWidth);
@@ -91,6 +124,67 @@
                 name + ".inputBuffer" + std::to_string(tid), "lines",
                 params.fetch2InputBufferSize));
     }
+
+    if (ifaMode > 2)
+        fatal("%s: ifa_mode must be 0, 1 or 2 (%d)\n", name, ifaMode);
+
+    if (ifaMode != 0) {
+        if (params.numThreads != 1) {
+            fatal("%s: the IFA only supports one thread (numThreads: %d)\n",
+                name, params.numThreads);
+        }
+
+        if (ifaBufferSize < 1) {
+            fatal("%s: ifa_buffer_size must be >= 1 (%d)\n", name,
+                ifaBufferSize);
+        }
+
+        if (ifaPassMemoryRefs && ifaHoistMemoryRefs) {
+            fatal("%s: ifa_pass_memory_refs and ifa_hoist_memory_refs"
+                " can't be used together\n", name);
+        }
+    }
+
+    /* Take the DAU's latency estimates from the Execute FU descriptions.
+     *  The FU used is the first that provides the OpClass, as in
+     *  Execute::issue */
+    const std::vector<MinorFU *> &fus = params.executeFuncUnits->funcUnits;
+
+    for (MinorFU *fu : fus) {
+        if (fu->opClasses->provides(IntAluOp)) {
+            if (!fu->timings.empty() &&
+                !fu->timings[0]->srcRegsRelativeLats.empty())
+            {
+                ifaSrcRelativeLatency =
+                    fu->timings[0]->srcRegsRelativeLats.back();
+            }
+            break;
+        }
+    }
+
+    for (int op_class = 0; op_class < Num_OpClasses; op_class++) {
+        for (int fu_index = 0; fu_index < fus.size(); fu_index++) {
+            MinorFU *fu = fus[fu_index];
+
+            if (fu->opClasses->provides(static_cast<OpClass>(op_class))) {
+                ifaFUOpLat[op_class] = fu->opLat + (fu->timings.empty() ?
+                    Cycles(0) : fu->timings[0]->extraAssumedLat);
+                if (!fu->timings.empty()) {
+                    ifaFURelLats[op_class] =
+                        fu->timings[0]->srcRegsRelativeLats;
+                }
+                ifaOpFU[op_class] = fu_index;
+                ifaFUIssueLat[op_class] = fu->issueLat;
+
+                Cycles latency = fu->opLat + (fu->timings.empty() ?
+                    Cycles(0) : fu->timings[0]->extraAssumedLat);
+
+                ifaOpLatency[op_class] = (latency > ifaSrcRelativeLatency ?
+                    Cycles(latency - ifaSrcRelativeLatency) : Cycles(0));
+                break;
+            }
+        }
+    }
 }
 
 const ForwardLineData *
@@ -236,6 +330,417 @@
     }
 }
 
+bool
+Fetch2::ifaIsBarrier(const MinorDynInstPtr &inst) const
+{
+    if (inst->isFault())
+        return true;
+
+    const StaticInstPtr &static_inst = inst->staticInst;
+
+    /* Control instructions are barriers because Execute discards everything
+     *  it receives after a mispredicted branch.  Things which can fault and
+     *  aren't memory references, or which act on more state than their
+     *  register operands, must stay in order for exceptions to be precise */
+    return static_inst->isControl() ||
+        static_inst->isSyscall() ||
+        static_inst->isSerializing() ||
+        static_inst->isNonSpeculative() ||
+        static_inst->isSquashAfter() ||
+        static_inst->isReadBarrier() ||
+        static_inst->isWriteBarrier() ||
+        static_inst->isAtomic() ||
+        static_inst->isMacroop() ||
+        static_inst->isMicroop() ||
+        static_inst->isQuiesce() ||
+        static_inst->isHtmCmd() ||
+        static_inst->isInvalid();
+}
+
+/** Do the register operands of a (dests or srcs) and b (dests or srcs)
+ *  have a register in common?  Invalid registers (such as RISC-V's x0)
+ *  carry no dependencies */
+static bool
+ifaRegsOverlap(const StaticInstPtr &a, bool a_dests,
+    const StaticInstPtr &b, bool b_dests)
+{
+    int num_a = (a_dests ? a->numDestRegs() : a->numSrcRegs());
+    int num_b = (b_dests ? b->numDestRegs() : b->numSrcRegs());
+
+    for (int i = 0; i < num_a; i++) {
+        const RegId &reg_a = (a_dests ? a->destRegIdx(i) : a->srcRegIdx(i));
+
+        if (reg_a.classValue() == InvalidRegClass)
+            continue;
+
+        for (int j = 0; j < num_b; j++) {
+            if (reg_a == (b_dests ? b->destRegIdx(j) : b->srcRegIdx(j)))
+                return true;
+        }
+    }
+
+    return false;
+}
+
+bool
+Fetch2::ifaSourcesReady(const MinorDynInstPtr &inst)
+{
+    if (inst->isFault())
+        return true;
+
+    const StaticInstPtr &static_inst = inst->staticInst;
+    ThreadID tid = inst->id.threadId;
+    auto *isa = cpu.getContext(tid)->getIsaPtr();
+    /* inst can reach issue in Execute no earlier than this */
+    Cycles arrival = ifaArrival;
+
+    for (int i = 0; i < static_inst->numSrcRegs(); i++) {
+        RegId reg = static_inst->srcRegIdx(i).flatten(*isa);
+        Scoreboard::Index index;
+
+        if (!ifaShadow.findIndex(reg, index))
+            continue;
+
+        /* Producers dispatched by Fetch2, including those not yet in
+         *  Execute, with their estimated latency */
+        if (ifaDAUSource != enums::IFAScoreboard &&
+            ifaShadow.returnCycle[index] > arrival)
+        {
+            return false;
+        }
+
+        /* Producers in Execute.  A memory reference which hasn't completed
+         *  is waited for whatever the shadow estimate (it may have
+         *  missed in the cache) */
+        if (ifaDAUSource != enums::IFAShadow) {
+            Execute::IFARegState state = execute.ifaRegState(tid, reg);
+
+            if (state.memPending)
+                return false;
+
+            if (ifaDAUSource == enums::IFAScoreboard && state.pending &&
+                state.returnCycle > arrival + ifaSrcRelativeLatency)
+            {
+                return false;
+            }
+        }
+    }
+
+    return true;
+}
+
+bool
+Fetch2::ifaCanPass(const MinorDynInstPtr &inst,
+    const std::vector<MinorDynInstPtr> &skipped) const
+{
+    const StaticInstPtr &static_inst = inst->staticInst;
+
+    /* Barriers only leave in program order */
+    if (ifaIsBarrier(inst))
+        return false;
+
+    /* Memory references normally leave in program order too.  This keeps
+     *  the order of memory accesses and makes a faulting access precise:
+     *  nothing older is left undispatched behind it.  When hoisting, a
+     *  memory reference can pass the head if nothing younger than the
+     *  head has been dispatched: if it faults, restarting from the head
+     *  executes nothing twice */
+    if (static_inst->isMemRef()) {
+        if (!ifaHoistMemoryRefs ||
+            ifaMaxDispatchedSeqNum >= skipped.front()->id.fetchSeqNum)
+        {
+            return false;
+        }
+    }
+
+    /* If an older memory reference which inst passes faults after inst has
+     *  committed, inst will be executed again when the fault returns.  That
+     *  is only safe if it doesn't overwrite one of its own sources, and
+     *  still adds to the committed instruction count */
+    bool can_pass_memory_refs = ifaPassMemoryRefs &&
+        !ifaRegsOverlap(static_inst, true, static_inst, false);
+
+    for (const MinorDynInstPtr &older : skipped) {
+        const StaticInstPtr &older_inst = older->staticInst;
+
+        if (older_inst->isMemRef() &&
+            (!can_pass_memory_refs || static_inst->isMemRef()))
+        {
+            return false;
+        }
+
+        if (ifaRegsOverlap(older_inst, true, static_inst, false) || /* RAW */
+            ifaRegsOverlap(older_inst, false, static_inst, true) || /* WAR */
+            ifaRegsOverlap(older_inst, true, static_inst, true)) /* WAW */
+        {
+            return false;
+        }
+    }
+
+    return true;
+}
+
+void
+Fetch2::ifaMarkupDispatch(const MinorDynInstPtr &inst)
+{
+    if (inst->isFault())
+        return;
+
+    const StaticInstPtr &static_inst = inst->staticInst;
+    auto *isa = cpu.getContext(inst->id.threadId)->getIsaPtr();
+    Cycles ready = ifaArrival +
+        (static_inst->isMemRef() ? ifaLoadLatency :
+            ifaOpLatency[static_inst->opClass()]);
+
+    for (int i = 0; i < static_inst->numDestRegs(); i++) {
+        RegId reg = static_inst->destRegIdx(i).flatten(*isa);
+        Scoreboard::Index index;
+
+        if (ifaShadow.findIndex(reg, index))
+            ifaShadow.returnCycle[index] = ready;
+    }
+}
+
+Cycles
+Fetch2::ifaEstimateIssue(const MinorDynInstPtr &inst) const
+{
+    /* In order: not before the last dispatched instruction */
+    Cycles issue = std::max(ifaModelLastIssue, ifaArrival);
+
+    if (!inst->isFault()) {
+        const StaticInstPtr &static_inst = inst->staticInst;
+        OpClass op_class = static_inst->opClass();
+        const std::vector<Cycles> &rel_lats = ifaFURelLats[op_class];
+        auto *isa = cpu.getContext(inst->id.threadId)->getIsaPtr();
+
+        /* As Scoreboard::canInstIssue: a source is available from its
+         *  returnCycle less the source's relative latency */
+        for (int i = 0; i < static_inst->numSrcRegs(); i++) {
+            RegId reg = static_inst->srcRegIdx(i).flatten(*isa);
+            Scoreboard::Index index;
+
+            if (!ifaShadow.findIndex(reg, index))
+                continue;
+
+            Cycles rel_lat = (rel_lats.empty() ? Cycles(0) :
+                rel_lats[std::min<size_t>(i, rel_lats.size() - 1)]);
+            Cycles ready = ifaShadow.returnCycle[index];
+
+            if (ready > issue + rel_lat)
+                issue = Cycles(ready - rel_lat);
+        }
+
+        int fu_index = ifaOpFU[op_class];
+        if (fu_index >= 0)
+            issue = std::max(issue, ifaModelFUFree[fu_index]);
+    }
+
+    /* Execute issues up to ifaIssueLimit instructions a cycle and stops
+     *  after a memory reference */
+    if (issue == ifaModelLastIssue &&
+        (ifaModelNumAtLast >= ifaIssueLimit || ifaModelMemAtLast))
+    {
+        issue = Cycles(issue + 1);
+    }
+
+    return issue;
+}
+
+void
+Fetch2::ifaModelDispatch(const MinorDynInstPtr &inst, Cycles issue_cycle)
+{
+    bool is_mem = !inst->isFault() && inst->staticInst->isMemRef();
+
+    if (issue_cycle > ifaModelLastIssue) {
+        ifaModelLastIssue = issue_cycle;
+        ifaModelNumAtLast = 1;
+        ifaModelMemAtLast = is_mem;
+    } else {
+        ifaModelNumAtLast++;
+        ifaModelMemAtLast = ifaModelMemAtLast || is_mem;
+    }
+
+    if (inst->isFault())
+        return;
+
+    const StaticInstPtr &static_inst = inst->staticInst;
+    OpClass op_class = static_inst->opClass();
+    auto *isa = cpu.getContext(inst->id.threadId)->getIsaPtr();
+
+    for (int i = 0; i < static_inst->numDestRegs(); i++) {
+        RegId reg = static_inst->destRegIdx(i).flatten(*isa);
+        Scoreboard::Index index;
+
+        if (ifaShadow.findIndex(reg, index)) {
+            ifaShadow.returnCycle[index] =
+                Cycles(issue_cycle + ifaFUOpLat[op_class]);
+        }
+    }
+
+    int fu_index = ifaOpFU[op_class];
+    if (fu_index >= 0 && ifaFUIssueLat[op_class] > 1) {
+        ifaModelFUFree[fu_index] =
+            Cycles(issue_cycle + ifaFUIssueLat[op_class]);
+    }
+}
+
+void
+Fetch2::ifaDispatch(ForwardInstData &insts_out)
+{
+    const Cycles now = cpu.curCycle();
+    unsigned int output_index = 0;
+    bool passed_head = false;
+
+    auto dispatchable = [&](const IFAEntry &entry) {
+        return entry.dispatchableCycle <= now;
+    };
+
+    /* Move an entry to the output, returning the entry after it */
+    auto dispatch = [&](std::deque<IFAEntry>::iterator entry,
+        bool out_of_order)
+    {
+        MinorDynInstPtr inst = entry->inst;
+
+        if (output_index == 0)
+            insts_out.resize(outputWidth);
+        insts_out.insts[output_index] = inst;
+        output_index++;
+
+        if (ifaIssueModel)
+            ifaModelDispatch(inst, ifaEstimateIssue(inst));
+        else
+            ifaMarkupDispatch(inst);
+        ifaMaxDispatchedSeqNum = std::max(ifaMaxDispatchedSeqNum,
+            inst->id.fetchSeqNum);
+        stats.ifaDispatched++;
+        if (out_of_order) {
+            stats.ifaDispatchedOutOfOrder++;
+            passed_head = true;
+        }
+
+        DPRINTF(Fetch, "IFA dispatching%s inst: %s\n",
+            (out_of_order ? " out of order" : ""), *inst);
+
+        return ifaBuffer.erase(entry);
+    };
+
+    if (ifaBuffer.empty() || !dispatchable(ifaBuffer.front()))
+        return;
+
+    /* Instructions dispatched now reach issue after the latches and, if
+     *  asked, after the instructions already queued in Decode's and
+     *  Execute's input buffers (each holding one cycle's worth of
+     *  instructions) */
+    ifaArrival = now + ifaDispatchToIssue;
+    if (ifaQueueAware) {
+        ThreadID tid = ifaBuffer.front().inst->id.threadId;
+
+        ifaArrival += Cycles(decodeInputBufferSize -
+            nextStageReserve[tid].unreservedRemainingSpace() +
+            execute.ifaInputBufferOccupancy(tid));
+    }
+
+    bool head_blocked = (ifaIssueModel ?
+        ifaEstimateIssue(ifaBuffer.front().inst) >
+            Cycles(std::max(ifaModelLastIssue, ifaArrival) + 1) :
+        !ifaSourcesReady(ifaBuffer.front().inst));
+
+    if (head_blocked)
+        stats.ifaHeadBlockedCycles++;
+
+    if (ifaMode == 2 && ifaIssueModel) {
+        /* List scheduling: each slot gets the legal instruction which would
+         *  issue earliest, the oldest on a tie */
+        while (output_index < outputWidth && !ifaBuffer.empty()) {
+            std::vector<MinorDynInstPtr> older;
+            auto best = ifaBuffer.end();
+            Cycles best_issue(0);
+            MinorDynInstPtr best_passed_head = nullptr;
+            unsigned int best_num_passed = 0;
+
+            for (auto entry = ifaBuffer.begin();
+                entry != ifaBuffer.end() && dispatchable(*entry); entry++)
+            {
+                const MinorDynInstPtr &inst = entry->inst;
+
+                if (older.empty() || ifaCanPass(inst, older)) {
+                    Cycles issue = ifaEstimateIssue(inst);
+
+                    if (best == ifaBuffer.end() || issue < best_issue) {
+                        best = entry;
+                        best_issue = issue;
+                        best_passed_head =
+                            (older.empty() ? nullptr : older.front());
+                        best_num_passed = older.size();
+                    }
+                }
+
+                /* Nothing younger can pass a barrier */
+                if (ifaIsBarrier(inst))
+                    break;
+                older.push_back(inst);
+            }
+
+            if (best == ifaBuffer.end())
+                break;
+
+            const MinorDynInstPtr inst = best->inst;
+
+            if (best_num_passed != 0 && inst->staticInst->isMemRef()) {
+                /* A hoisted memory reference: a fault in it must restart
+                 *  from the oldest instruction it passed */
+                set(inst->ifaRestartPC, best_passed_head->pc);
+                stats.ifaHoisted++;
+            }
+            stats.ifaSkipped += best_num_passed;
+
+            dispatch(best, best_num_passed != 0);
+        }
+    } else if (ifaMode == 2) {
+        /* Older instructions which the scan has left in the FEB */
+        std::vector<MinorDynInstPtr> skipped;
+        auto entry = ifaBuffer.begin();
+
+        while (entry != ifaBuffer.end() && dispatchable(*entry) &&
+            output_index < outputWidth)
+        {
+            const MinorDynInstPtr inst = entry->inst;
+
+            if (ifaSourcesReady(inst) &&
+                (skipped.empty() || ifaCanPass(inst, skipped)))
+            {
+                if (!skipped.empty() && inst->staticInst->isMemRef()) {
+                    /* A hoisted memory reference: a fault in it must
+                     *  restart from the oldest instruction it passed */
+                    set(inst->ifaRestartPC, skipped.front()->pc);
+                    stats.ifaHoisted++;
+                }
+                entry = dispatch(entry, !skipped.empty());
+            } else if (ifaIsBarrier(inst)) {
+                /* Nothing younger can pass a barrier */
+                break;
+            } else {
+                DPRINTF(Fetch, "IFA skipping inst: %s\n", *inst);
+                skipped.push_back(inst);
+                stats.ifaSkipped++;
+                entry++;
+            }
+        }
+    }
+
+    /* Fill the rest of the output in program order.  This is all of
+     *  dispatch in mode 1 and, in mode 2, makes sure the IFA never sends
+     *  fewer instructions than in-order dispatch would */
+    while (output_index < outputWidth && !ifaBuffer.empty() &&
+        dispatchable(ifaBuffer.front()))
+    {
+        dispatch(ifaBuffer.begin(), false);
+    }
+
+    if (head_blocked && passed_head)
+        stats.ifaReorderCycles++;
+}
+
 void
 Fetch2::evaluate()
 {
@@ -260,6 +765,25 @@
             " has arrived\n");
         dumpAllInput(branch_inp.threadId);
         fetchInfo[branch_inp.threadId].havePC = false;
+
+        /* Instructions in the FEB from the old stream would only be
+         *  discarded by Execute */
+        if (ifaMode != 0 && ifaFlushOnStreamChange) {
+            auto old_stream = std::remove_if(ifaBuffer.begin(),
+                ifaBuffer.end(), [&](const IFAEntry &entry) {
+                    return entry.inst->id.streamSeqNum !=
+                        branch_inp.newStreamSeqNum;
+                });
+            unsigned int num_flushed = ifaBuffer.end() - old_stream;
+
+            if (num_flushed != 0) {
+                DPRINTF(Fetch, "IFA discarding %d FEB entries\n",
+                    num_flushed);
+                stats.ifaFlushes++;
+                stats.ifaFlushedInsts += num_flushed;
+                ifaBuffer.erase(old_stream, ifaBuffer.end());
+            }
+        }
     }
 
     assert(insts_out.isBubble());
@@ -303,13 +827,20 @@
 
         unsigned int output_index = 0;
 
-        /* Pack instructions into the output while we can.  This may involve
-         * using more than one input line.  Note that lineWidth will be 0
-         * for faulting lines */
+        /* Number of instructions put into the FEB this cycle */
+        unsigned int ifa_num_decoded = 0;
+
+        /* Pack instructions into the output (or, with the IFA, the FEB)
+         * while we can.  This may involve using more than one input line.
+         * Note that lineWidth will be 0 for faulting lines */
         while (line_in &&
             (line_in->isFault() ||
                 fetch_info.inputIndex < line_in->lineWidth) && /* More input */
-            output_index < outputWidth && /* More output to fill */
+            (ifaMode == 0 ?
+                output_index < outputWidth : /* More output to fill */
+                (ifaBuffer.size() < ifaBufferSize && /* FEB space */
+                    (ifaDecodeWidth == 0 ||
+                        ifa_num_decoded < ifaDecodeWidth))) &&
             prediction.isBubble() /* No predicted branch */)
         {
             ThreadContext *thread = cpu.getContext(line_in->id.threadId);
@@ -480,13 +1011,21 @@
                 /* Step to next sequence number */
                 fetch_info.fetchSeqNum++;
 
-                /* Correctly size the output before writing */
-                if (output_index == 0) {
-                    insts_out.resize(outputWidth);
+                if (ifaMode != 0) {
+                    /* The IFA dispatches from the FEB below */
+                    ifaBuffer.push_back(
+                        IFAEntry{dyn_inst, cpu.curCycle() + ifaLatency});
+                    ifa_num_decoded++;
+                } else {
+                    /* Correctly size the output before writing */
+                    if (output_index == 0) {
+                        insts_out.resize(outputWidth);
+                    }
+                    /* Pack the generated dynamic instruction into the
+                     *  output */
+                    insts_out.insts[output_index] = dyn_inst;
+                    output_index++;
                 }
-                /* Pack the generated dynamic instruction into the output */
-                insts_out.insts[output_index] = dyn_inst;
-                output_index++;
 
                 /* Output MinorTrace instruction info for
                  *  pre-microop decomposition macroops */
@@ -529,6 +1068,16 @@
             }
         }
 
+        if (ifaMode != 0) {
+            if (ifaBuffer.size() >= ifaBufferSize && getInput(tid))
+                stats.ifaBufferFullCycles++;
+
+            /* With ifaFillWhenBlocked, this thread may only have been
+             *  scheduled to fill the FEB */
+            if (!fetch_info.blocked)
+                ifaDispatch(insts_out);
+        }
+
         /* The rest of the output (if any) should already have been packed
          *  with bubble instructions by insts_out's initialisation */
     }
@@ -551,12 +1100,19 @@
      *  mark stage as active */
     for (ThreadID i = 0; i < cpu.numThreads; i++)
     {
-        if (getInput(i) && nextStageReserve[i].canReserve()) {
+        if (((getInput(i) || !ifaBuffer.empty()) &&
+                nextStageReserve[i].canReserve()) ||
+            (ifaFillWhenBlocked && getInput(i) &&
+                ifaBuffer.size() < ifaBufferSize))
+        {
             cpu.activityRecorder->activateStage(Pipeline::Fetch2StageId);
             break;
         }
     }
 
+    if (ifaMode != 0)
+        stats.ifaOccupancy.sample(ifaBuffer.size());
+
     /* Make sure the input (if any left) is pushed */
     if (!inp.outputWire->isBubble())
         inputBuffer[inp.outputWire->id.threadId].pushTail();
@@ -583,7 +1139,11 @@
     }
 
     for (auto tid : priority_list) {
-        if (getInput(tid) && !fetchInfo[tid].blocked) {
+        if (((getInput(tid) || !ifaBuffer.empty()) &&
+                !fetchInfo[tid].blocked) ||
+            (ifaFillWhenBlocked && getInput(tid) &&
+                ifaBuffer.size() < ifaBufferSize))
+        {
             threadPriority = tid;
             return tid;
         }
@@ -600,11 +1160,13 @@
             return false;
     }
 
-    return (*inp.outputWire).isBubble() &&
+    return ifaBuffer.empty() &&
+           (*inp.outputWire).isBubble() &&
            (*predictionOut.inputWire).isBubble();
 }
 
-Fetch2::Fetch2Stats::Fetch2Stats(MinorCPU *cpu)
+Fetch2::Fetch2Stats::Fetch2Stats(MinorCPU *cpu,
+    unsigned int ifa_buffer_size)
     : statistics::Group(cpu, "fetch2"),
       ADD_STAT(totalInstructions, statistics::units::Count::get(),
                "Total number of instructions successfully decoded"),
@@ -619,7 +1181,33 @@
       ADD_STAT(storeInstructions, statistics::units::Count::get(),
                "Number of memory store instructions successfully decoded"),
       ADD_STAT(amoInstructions, statistics::units::Count::get(),
-               "Number of memory atomic instructions successfully decoded")
+               "Number of memory atomic instructions successfully decoded"),
+      ADD_STAT(ifaDispatched, statistics::units::Count::get(),
+               "Number of instructions dispatched from the IFA FEB"),
+      ADD_STAT(ifaDispatchedOutOfOrder, statistics::units::Count::get(),
+               "Number of instructions dispatched from the IFA FEB while an"
+               " older instruction stayed in it"),
+      ADD_STAT(ifaHoisted, statistics::units::Count::get(),
+               "Number of memory references dispatched from the IFA FEB"
+               " before older instructions"),
+      ADD_STAT(ifaSkipped, statistics::units::Count::get(),
+               "Number of times the IFA dispatch scan left an instruction in"
+               " the FEB to look at younger ones"),
+      ADD_STAT(ifaHeadBlockedCycles, statistics::units::Cycle::get(),
+               "Number of cycles in which the oldest FEB instruction could"
+               " be dispatched but the DAU found it not ready"),
+      ADD_STAT(ifaReorderCycles, statistics::units::Cycle::get(),
+               "Number of cycles in which the oldest FEB instruction was not"
+               " ready and a younger one was dispatched before it"),
+      ADD_STAT(ifaBufferFullCycles, statistics::units::Cycle::get(),
+               "Number of cycles in which input could not be decoded"
+               " because the IFA FEB was full"),
+      ADD_STAT(ifaFlushes, statistics::units::Count::get(),
+               "Number of stream changes which discarded IFA FEB entries"),
+      ADD_STAT(ifaFlushedInsts, statistics::units::Count::get(),
+               "Number of IFA FEB entries discarded on stream changes"),
+      ADD_STAT(ifaOccupancy, statistics::units::Count::get(),
+               "Number of IFA FEB entries at the end of each cycle")
 {
     totalInstructions.flags(statistics::total);
     intInstructions.flags(statistics::total);
@@ -628,6 +1216,8 @@
     loadInstructions.flags(statistics::total);
     storeInstructions.flags(statistics::total);
     amoInstructions.flags(statistics::total);
+
+    ifaOccupancy.init(0, ifa_buffer_size, 1);
 }
 
 void
--- a/src/cpu/minor/execute.hh
+++ b/src/cpu/minor/execute.hh
@@ -124,6 +124,23 @@
     /** Scoreboard of instruction dependencies */
     std::vector<Scoreboard> scoreboard;
 
+    /** Copy of scoreboard taken at the start of each cycle for the IFA in
+     *  Fetch2.  Execute is evaluated before Fetch2 in the same cycle, so
+     *  reading scoreboard directly would give Fetch2 the effect of this
+     *  cycle's issue and commit with no delay */
+    std::vector<Scoreboard> ifaScoreboardSnapshot;
+
+    /** Keep ifaScoreboardSnapshot up to date */
+    bool ifaNeedsScoreboard;
+
+    /** Number of input buffer slots (occupied or reserved) at the start of
+     *  this cycle, and at the end of this cycle, for the IFA */
+    std::vector<unsigned int> ifaInputOccupancy;
+    std::vector<unsigned int> ifaNextInputOccupancy;
+
+    /** Capacity of each input buffer */
+    unsigned int inputBufferSize;
+
     /** The execution functional units */
     std::vector<FUPipeline *> funcUnits;
 
@@ -213,6 +230,10 @@
   protected:
     friend std::ostream &operator <<(std::ostream &os, DrainState state);
 
+    /** If the IFA dispatched inst before older instructions, make a fault
+     *  in inst restart execution from the oldest of them */
+    void ifaPrepareFault(MinorDynInstPtr inst, ThreadContext *thread);
+
     /** Get a piece of data to work on from the inputBuffer, or 0 if there
      *  is no data. */
     const ForwardInstData *getInput(ThreadID tid);
@@ -347,6 +368,33 @@
     /** Pass on input/buffer data to the output if you can */
     void evaluate();
 
+    /** State of a register in the scoreboard, as seen by the IFA */
+    struct IFARegState
+    {
+        /** An in-flight instruction will write the register */
+        bool pending = false;
+
+        /** The most recent writer is a memory reference which has not
+         *  completed, so its result time can't be predicted */
+        bool memPending = false;
+
+        /** Scoreboard's estimate of when the result will be available
+         *  (see Scoreboard::returnCycle) */
+        Cycles returnCycle = Cycles(0);
+    };
+
+    /** The state of a (flattened) register in the scoreboard at the start
+     *  of the current cycle, for the IFA's dependency analysis in Fetch2 */
+    IFARegState ifaRegState(ThreadID thread_id, const RegId &reg) const;
+
+    /** Number of input buffer slots, occupied or reserved by instructions
+     *  on their way from Decode, at the start of the current cycle */
+    unsigned int
+    ifaInputBufferOccupancy(ThreadID thread_id) const
+    {
+        return ifaInputOccupancy[thread_id];
+    }
+
     void minorTrace() const;
 
     /** After thread suspension, has Execute been drained of in-flight
@@ -357,10 +405,26 @@
     unsigned int drain();
     void drainResume();
 
+    /** Why issue stopped in a cycle */
+    enum IssueStop
+    {
+        IssueStopFull, /* Issued executeIssueLimit instructions */
+        IssueStopNoInput, /* No (more) instructions to issue */
+        IssueStopOperands, /* Next instruction's sources not ready */
+        IssueStopFU, /* No capable FU could accept the next instruction */
+        IssueStopMemLimit, /* executeMemoryIssueLimit reached */
+        IssueStopOther,
+        NumIssueStop
+    };
+
+    /** Why the last call to issue stopped */
+    IssueStop issueStop;
+
     struct IssueStats : public statistics::Group
     {
         IssueStats(MinorCPU *cpu);
         statistics::Vector2d issuedInstType;
+        statistics::Vector issueStopCycles;
     } issueStats;
 };
 
--- a/src/cpu/minor/execute.cc
+++ b/src/cpu/minor/execute.cc
@@ -79,6 +79,11 @@
       setTraceTimeOnIssue(params.executeSetTraceTimeOnIssue),
       allowEarlyMemIssue(params.executeAllowEarlyMemoryIssue),
       noCostFUIndex(fuDescriptions.funcUnits.size() + 1),
+      ifaNeedsScoreboard(params.ifa_mode != 0 &&
+          params.ifa_dau_source != enums::IFAShadow),
+      ifaInputOccupancy(params.numThreads, 0),
+      ifaNextInputOccupancy(params.numThreads, 0),
+      inputBufferSize(params.executeInputBufferSize),
       lsq(name_ + ".lsq", name_ + ".dcache_port", cpu_, *this,
           params.executeMaxAccessesInMemory, params.executeMemoryWidth,
           params.executeLSQRequestsQueueSize,
@@ -90,6 +95,7 @@
       interruptPriority(0),
       issuePriority(0),
       commitPriority(0),
+      issueStop(IssueStopNoInput),
       issueStats(&cpu_)
 {
     if (commitLimit < 1) {
@@ -180,6 +186,10 @@
 
         /* Scoreboards */
         scoreboard.emplace_back(name_ + ".scoreboard" + tid_str, regClasses);
+        if (ifaNeedsScoreboard) {
+            ifaScoreboardSnapshot.emplace_back(
+                name_ + ".ifaScoreboardSnapshot" + tid_str, regClasses);
+        }
 
         /* In-flight instruction records */
         executeInfo[tid].inFlightInsts =  new Queue<QueuedInst,
@@ -353,6 +363,7 @@
             /* Take the fault raised during the TLB/memory access */
             fault = inst->translationFault;
 
+            ifaPrepareFault(inst, thread);
             fault->invoke(thread, inst->staticInst);
         }
     } else if (!packet) {
@@ -385,6 +396,7 @@
             /* Invoke fault created by instruction completion */
             DPRINTF(MinorMem, "Fault in memory completeAcc: %s\n",
                 fault->name());
+            ifaPrepareFault(inst, thread);
             fault->invoke(thread, inst->staticInst);
         } else {
             /* Stores need to be pushed into the store buffer to finish
@@ -546,8 +558,14 @@
     ExecuteThreadInfo &thread = executeInfo[thread_id];
 
     /* Early termination if we have no instructions */
-    if (!insts_in)
+    if (!insts_in) {
+        issueStop = IssueStopNoInput;
         return 0;
+    }
+
+    /* Why the instruction being tried could not be issued */
+    bool failed_operands = false;
+    bool failed_fu = false;
 
     /* Start from the first FU */
     unsigned int fu_index = 0;
@@ -591,6 +609,8 @@
             /* Try and issue an instruction into an FU, assume we didn't and
              * fix that in the loop */
             issued = false;
+            failed_operands = false;
+            failed_fu = false;
 
             /* Try FU from 0 each instruction */
             fu_index = 0;
@@ -637,6 +657,8 @@
 
                 } else if (!fu_is_capable || fu->alreadyPushed()) {
                     /* Skip */
+                    if (fu_is_capable)
+                        failed_fu = true;
                     if (!fu_is_capable) {
                         DPRINTF(MinorExecute, "Can't issue as FU: %d isn't"
                             " capable\n", fu_index);
@@ -645,10 +667,12 @@
                             " already busy\n", fu_index);
                     }
                 } else if (fu->stalled) {
+                    failed_fu = true;
                     DPRINTF(MinorExecute, "Can't issue inst: %s into FU: %d,"
                         " it's stalled\n",
                         *inst, fu_index);
                 } else if (!fu->canInsert()) {
+                    failed_fu = true;
                     DPRINTF(MinorExecute, "Can't issue inst: %s to busy FU"
                         " for another: %d cycles\n",
                         *inst, fu->cyclesBeforeInsert());
@@ -671,6 +695,7 @@
                         src_latencies, cant_forward_from_fu_indices,
                         cpu.curCycle(), cpu.getContext(thread_id)))
                     {
+                        failed_operands = true;
                         DPRINTF(MinorExecute, "Can't issue inst: %s yet\n",
                             *inst);
                     } else {
@@ -841,6 +866,19 @@
         num_insts_issued != issueLimit && /* Still allowed to issue */
         num_mem_insts_issued != memoryIssueLimit);
 
+    if (!issued) {
+        issueStop = (failed_operands ? IssueStopOperands :
+            (failed_fu ? IssueStopFU : IssueStopOther));
+    } else if (num_insts_issued == issueLimit) {
+        issueStop = IssueStopFull;
+    } else if (insts_in && thread.inputIndex < insts_in->width() &&
+        num_mem_insts_issued == memoryIssueLimit)
+    {
+        issueStop = IssueStopMemLimit;
+    } else {
+        issueStop = IssueStopNoInput;
+    }
+
     return num_insts_issued;
 }
 
@@ -1009,6 +1047,7 @@
             } else {
                 DPRINTF(MinorExecute, "Fault in execute: %s\n",
                     fault->name());
+                ifaPrepareFault(inst, thread);
                 fault->invoke(thread, NULL);
 
                 tryToBranch(inst, fault, branch);
@@ -1497,6 +1536,21 @@
 
     unsigned int num_issued = 0;
 
+    /* Remember the scoreboard and input buffer as they were at the end of
+     *  the last cycle for Fetch2's IFA, which is evaluated after this stage */
+    ifaInputOccupancy = ifaNextInputOccupancy;
+    if (ifaNeedsScoreboard) {
+        for (ThreadID tid = 0; tid < cpu.numThreads; tid++) {
+            Scoreboard &snapshot = ifaScoreboardSnapshot[tid];
+
+            snapshot.numResults = scoreboard[tid].numResults;
+            snapshot.numUnpredictableResults =
+                scoreboard[tid].numUnpredictableResults;
+            snapshot.fuIndices = scoreboard[tid].fuIndices;
+            snapshot.returnCycle = scoreboard[tid].returnCycle;
+        }
+    }
+
     /* Do all the cycle-wise activities for dcachePort here to potentially
      *  free up input spaces in the LSQ's requests queue */
     lsq.step();
@@ -1567,6 +1621,9 @@
             DPRINTF(MinorExecute, "Attempting to issue [tid:%d]\n",
                     issue_tid);
             num_issued = issue(issue_tid);
+            issueStats.issueStopCycles[issueStop]++;
+        } else {
+            issueStats.issueStopCycles[IssueStopNoInput]++;
         }
 
     }
@@ -1672,6 +1729,11 @@
     /* Make sure the input (if any left) is pushed */
     if (!inp.outputWire->isBubble())
         inputBuffer[inp.outputWire->threadId].pushTail();
+
+    for (ThreadID tid = 0; tid < cpu.numThreads; tid++) {
+        ifaNextInputOccupancy[tid] = inputBufferSize -
+            inputBuffer[tid].unreservedRemainingSpace();
+    }
 }
 
 ThreadID
@@ -1721,6 +1783,43 @@
 }
 
 void
+Execute::ifaPrepareFault(MinorDynInstPtr inst, ThreadContext *thread)
+{
+    /* Instructions older than inst which the IFA let it pass have not been
+     *  executed.  Fault from the oldest of them, as if it had faulted:
+     *  everything older has committed and nothing younger has */
+    if (inst->ifaRestartPC) {
+        DPRINTF(MinorExecute, "IFA restarting fault in inst: %s from: %s\n",
+            *inst, *inst->ifaRestartPC);
+        thread->pcState(*inst->ifaRestartPC);
+    }
+}
+
+Execute::IFARegState
+Execute::ifaRegState(ThreadID thread_id, const RegId &reg) const
+{
+    assert(ifaNeedsScoreboard);
+
+    const Scoreboard &snapshot = ifaScoreboardSnapshot[thread_id];
+    IFARegState state;
+    Scoreboard::Index index;
+
+    if (snapshot.findIndex(reg, index) && snapshot.numResults[index] != 0) {
+        int fu_index = snapshot.fuIndices[index];
+
+        state.pending = true;
+        /* Memory references hold their destinations in the scoreboard until
+         *  they commit, so this is exact even for cache misses */
+        state.memPending = snapshot.numUnpredictableResults[index] != 0 ||
+            (fu_index >= 0 && fu_index < numFuncUnits &&
+                funcUnits[fu_index]->provides(MemReadOp));
+        state.returnCycle = snapshot.returnCycle[index];
+    }
+
+    return state;
+}
+
+void
 Execute::minorTrace() const
 {
     std::ostringstream insts;
@@ -1965,8 +2064,23 @@
 Execute::IssueStats::IssueStats(MinorCPU *cpu)
     : statistics::Group(cpu),
       ADD_STAT(issuedInstType, statistics::units::Count::get(),
-               "Number of instructions issued per FU type, per thread")
-{
+               "Number of instructions issued per FU type, per thread"),
+      ADD_STAT(issueStopCycles, statistics::units::Cycle::get(),
+               "Number of cycles by why Execute stopped issuing: Full ="
+               " issued executeIssueLimit instructions, NoInput = nothing"
+               " (more) to issue, Operands = sources of the next instruction"
+               " not ready, FU = no FU could take it, MemLimit ="
+               " executeMemoryIssueLimit reached")
+{
+    issueStopCycles.init(NumIssueStop)
+        .subname(IssueStopFull, "Full")
+        .subname(IssueStopNoInput, "NoInput")
+        .subname(IssueStopOperands, "Operands")
+        .subname(IssueStopFU, "FU")
+        .subname(IssueStopMemLimit, "MemLimit")
+        .subname(IssueStopOther, "Other")
+        .flags(statistics::total | statistics::pdf);
+
     issuedInstType.init(cpu->numThreads, enums::Num_OpClass)
         .flags(statistics::total | statistics::pdf | statistics::dist);
     issuedInstType.ysubnames(enums::OpClassStrings);
--- a/src/cpu/minor/scoreboard.hh
+++ b/src/cpu/minor/scoreboard.hh
@@ -133,7 +133,7 @@
     /** Sets scoreboard_index to the index into numResults of the
      *  given register index.  Returns true if the given register
      *  is in the scoreboard and false if it isn't */
-    bool findIndex(const RegId& reg, Index &scoreboard_index);
+    bool findIndex(const RegId& reg, Index &scoreboard_index) const;
 
     /** Mark up an instruction's effects by incrementing
      *  numResults counts.  If mark_unpredictable is true, the inst's
--- a/src/cpu/minor/scoreboard.cc
+++ b/src/cpu/minor/scoreboard.cc
@@ -48,7 +48,7 @@
 {
 
 bool
-Scoreboard::findIndex(const RegId& reg, Index &scoreboard_index)
+Scoreboard::findIndex(const RegId& reg, Index &scoreboard_index) const
 {
     bool ret = false;
 
--- a/src/cpu/minor/pipeline.cc
+++ b/src/cpu/minor/pipeline.cc
@@ -72,7 +72,7 @@
         dToE.output(), eToF1.input()),
     decode(cpu.name() + ".decode", cpu, params,
         f2ToD.output(), dToE.input(), execute.inputBuffer),
-    fetch2(cpu.name() + ".fetch2", cpu, params,
+    fetch2(cpu.name() + ".fetch2", cpu, params, execute,
         f1ToF2.output(), eToF1.output(), f2ToF1.input(), f2ToD.input(),
         decode.inputBuffer),
     fetch1(cpu.name() + ".fetch1", cpu, params,
--- a/configs/deprecated/example/se.py
+++ b/configs/deprecated/example/se.py
@@ -134,6 +134,76 @@
 Options.addCommonOptions(parser)
 Options.addSEOptions(parser)
 
+parser.add_argument(
+    "--ifa-mode",
+    type=int,
+    default=0,
+    choices=[0, 1, 2],
+    help="Instruction Flow Amplifier (IFA) in the MinorCPU Fetch2 stage:"
+    " 0 = off (baseline), 1 = front-end buffer with in-order dispatch,"
+    " 2 = front-end buffer with out-of-order dispatch",
+)
+parser.add_argument(
+    "--ifa-buffer-size", type=int, default=16, help="IFA FEB entries"
+)
+parser.add_argument(
+    "--ifa-decode-width",
+    type=int,
+    default=0,
+    help="Instructions decoded into the IFA FEB per cycle (0: no limit)",
+)
+parser.add_argument(
+    "--ifa-latency",
+    type=int,
+    default=0,
+    help="Cycles an instruction spends in the IFA FEB before dispatch",
+)
+parser.add_argument(
+    "--ifa-dau-source",
+    default="IFAHybrid",
+    choices=["IFAShadow", "IFAScoreboard", "IFAHybrid"],
+    help="Where the IFA DAU finds in-flight producers",
+)
+parser.add_argument(
+    "--ifa-load-latency",
+    type=int,
+    default=4,
+    help="Load-to-use latency assumed by the IFA shadow scoreboard",
+)
+parser.add_argument(
+    "--ifa-issue-model",
+    action=argparse.BooleanOptionalAction,
+    default=True,
+    help="Schedule IFA dispatch with a model of Execute's in-order issue"
+    " (otherwise oldest ready first)",
+)
+parser.add_argument(
+    "--ifa-hoist-memory-refs",
+    action="store_true",
+    help="Let memory references pass older independent instructions",
+)
+parser.add_argument(
+    "--ifa-fill-when-blocked",
+    action=argparse.BooleanOptionalAction,
+    default=True,
+    help="Keep filling the IFA FEB while Decode is full",
+)
+parser.add_argument(
+    "--ifa-queue-aware",
+    action="store_true",
+    help="Include queued instructions in the IFA DAU's arrival estimate",
+)
+parser.add_argument(
+    "--ifa-pass-memory-refs",
+    action="store_true",
+    help="Let re-executable instructions pass older memory references",
+)
+parser.add_argument(
+    "--ifa-no-flush",
+    action="store_true",
+    help="Don't discard IFA FEB entries on stream changes",
+)
+
 if "--ruby" in sys.argv:
     Ruby.define_options(parser)
 
@@ -221,6 +291,24 @@
 for cpu in system.cpu:
     cpu.clk_domain = system.cpu_clk_domain
 
+# Instruction Flow Amplifier (MinorCPU only)
+for cpu in system.cpu:
+    if "ifa_mode" in cpu._params:
+        cpu.ifa_mode = args.ifa_mode
+        cpu.ifa_buffer_size = args.ifa_buffer_size
+        cpu.ifa_decode_width = args.ifa_decode_width
+        cpu.ifa_latency = args.ifa_latency
+        cpu.ifa_dau_source = args.ifa_dau_source
+        cpu.ifa_load_latency = args.ifa_load_latency
+        cpu.ifa_flush_on_stream_change = not args.ifa_no_flush
+        cpu.ifa_pass_memory_refs = args.ifa_pass_memory_refs
+        cpu.ifa_hoist_memory_refs = args.ifa_hoist_memory_refs
+        cpu.ifa_issue_model = args.ifa_issue_model
+        cpu.ifa_fill_when_blocked = args.ifa_fill_when_blocked
+        cpu.ifa_queue_aware = args.ifa_queue_aware
+    elif args.ifa_mode != 0:
+        fatal(f"--ifa-mode needs a MinorCPU, not {type(cpu).__name__}")
+
 if ObjectList.is_kvm_cpu(CPUClass) or ObjectList.is_kvm_cpu(FutureClass):
     if buildEnv["USE_X86_ISA"]:
         system.kvm_vm = KvmVM()
```
