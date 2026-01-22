import json
import sys

# Uso: python fix_json.py config.json config_fixed.json

def fix_keys(data):
    # Se for dicionário, varre as chaves
    if isinstance(data, dict):
        new_data = {}
        for k, v in data.items():
            # EXEMPLO: Renomear chaves que o McPAT antigo procura
            # O gem5 novo pode chamar de 'data_cache', o antigo 'dcache'
            new_key = k
            if k == "data_cache":
                new_key = "dcache"
            elif k == "instruction_cache":
                new_key = "icache"
            
            # Recursão para corrigir dentro dos objetos
            new_data[new_key] = fix_keys(v)
        return new_data
    
    # Se for lista, varre os itens
    elif isinstance(data, list):
        return [fix_keys(item) for item in data]
    
    return data

input_file = sys.argv[1]
output_file = sys.argv[2]

with open(input_file, 'r') as f:
    config = json.load(f)

# Aplica as correções
fixed_config = fix_keys(config)

with open(output_file, 'w') as f:
    json.dump(fixed_config, f, indent=4)

print(f"Arquivo convertido: {output_file}")