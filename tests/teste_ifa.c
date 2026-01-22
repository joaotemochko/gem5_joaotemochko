// teste_ifa.c
// Compilar com: riscv64-unknown-elf-gcc -static teste_ifa.c -o teste_ifa
// (Ou use o compilador da arquitetura que você compilou o gem5, ex: arm-linux-gnueabihf-gcc)

#include <stdio.h>
#include <stdlib.h>

#define N 128 // Tamanho suficiente para estourar L1 Cache e gerar stalls de memória

int main() {
    int i, j, k;
    volatile int soma = 0; // 'volatile' impede o compilador de otimizar e remover a dependência
    int A[N][N], B[N][N], C[N][N];

    // 1. Inicialização (Gera tráfego de memória)
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            A[i][j] = i + j;
            B[i][j] = i * j;
        }
    }

    // 2. Carga Pesada (Matmul - Gera Load-to-Use Stalls)
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            C[i][j] = 0;
            for (k = 0; k < N; k++) {
                // Dependência RAW clássica e Stalls de Memória aqui
                C[i][j] += A[i][k] * B[k][j]; 
            }
        }
    }

    // 3. Dependência Serial Estrita (Para testar o pior caso In-Order)
    for (i = 0; i < N; i++) {
        // Cada iteração depende estritamente do valor anterior de 'soma'
        soma = soma + C[i][i];
    }

    printf("Teste Concluido. Checksum: %d\n", soma);
    return 0;
}