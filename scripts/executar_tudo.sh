#!/usr/bin/env bash
# CardioIA - Fase 2 | executa as duas partes de ponta a ponta.
#
# Uso:  bash scripts/executar_tudo.sh
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$RAIZ"

echo "=============================================================="
echo " CardioIA - Fase 2 | Diagnostico Automatizado"
echo "=============================================================="
echo

echo ">> Verificando dependencias..."
python3 - <<'PY'
import importlib.util
import sys
faltando = [m for m in ("pandas", "numpy", "sklearn", "matplotlib")
            if importlib.util.find_spec(m) is None]
if faltando:
    sys.exit("Faltam pacotes: " + ", ".join(faltando) +
             "\nRode: pip install -r config/requirements.txt")
print("   ok - pandas, numpy, scikit-learn e matplotlib disponiveis")
PY
echo

echo ">> PARTE 1 - extracao de sintomas e sugestao de diagnostico"
echo "--------------------------------------------------------------"
mkdir -p document/other
python3 src/parte1_extracao/extrator_sintomas.py \
    --csv document/other/diagnosticos.csv \
    | tee document/other/relatorio_parte1.txt
echo

echo ">> PARTE 2 - classificador de risco (notebook)"
echo "--------------------------------------------------------------"
if python3 -c "import nbconvert" 2>/dev/null; then
    python3 -m nbconvert --to notebook --execute --inplace \
        src/parte2_classificador/classificador_risco.ipynb \
        --ExecutePreprocessor.timeout=300
    python3 -m nbconvert --to html \
        src/parte2_classificador/classificador_risco.ipynb \
        --output-dir document
    echo "   notebook executado e exportado para document/classificador_risco.html"
else
    echo "   nbconvert nao instalado - abra o notebook manualmente:"
    echo "   jupyter notebook src/parte2_classificador/classificador_risco.ipynb"
fi
echo

echo "=============================================================="
echo " Concluido."
echo "   Parte 1 .... document/other/relatorio_parte1.txt"
echo "               document/other/diagnosticos.csv"
echo "   Parte 2 .... document/classificador_risco.html"
echo "=============================================================="
