#!/usr/bin/env python3
"""
CardioIA - Fase 2 | Parte 1
Extracao de sintomas a partir de relatos em texto livre e sugestao de
diagnostico com base em um mapa de conhecimento (ontologia simples).

Pipeline:
    relato (.txt)
      -> normalizacao (minusculas, sem acento)
      -> casamento de expressoes (dados/sinonimos.csv) -> sintomas canonicos
      -> pontuacao das regras (dados/mapa_conhecimento.csv)
      -> ranking de diagnosticos + nivel de urgencia + justificativa

Uso:
    python src/extrator_sintomas.py
    python src/extrator_sintomas.py --frase "sinto dor no peito ao subir escadas"
    python src/extrator_sintomas.py --csv saidas/diagnosticos.csv
"""

from __future__ import annotations

from pathlib import Path
import argparse
import csv
import sys
import unicodedata

def achar_pasta_dados(nome: str = "dados") -> Path:
    """Sobe a arvore de diretorios ate encontrar a pasta de dados.

    Assim o script roda a partir da raiz do repositorio, de dentro de src/
    ou da propria pasta parte1_extracao/, sem precisar de caminho fixo.
    """
    inicio = Path(__file__).resolve().parent
    for pasta in [inicio, *inicio.parents]:
        candidato = pasta / nome
        if candidato.is_dir():
            return candidato
    raise FileNotFoundError(f"Pasta '{nome}' nao encontrada acima de {inicio}")


DADOS = achar_pasta_dados()
FRASES = DADOS / "frases_pacientes.txt"
MAPA = DADOS / "mapa_conhecimento.csv"
SINONIMOS = DADOS / "sinonimos.csv"

# Uma regra so "fecha" quando os DOIS sintomas da linha aparecem no relato.
# Casamento parcial vale pouco, para evitar que um sintoma generico
# ("dor no peito") puxe sozinho um diagnostico grave.
PESO_COMPLETO = 2.0
PESO_PARCIAL = 0.5

URGENCIA_TEXTO = {
    "emergencia": "EMERGENCIA - encaminhar imediatamente",
    "urgente": "URGENTE - avaliacao medica em ate 24h",
    "rotina": "ROTINA - agendar consulta eletiva",
}


def normalizar(texto: str) -> str:
    """Minusculas, sem acento, espacos colapsados."""
    texto = texto.lower().strip()
    texto = unicodedata.normalize("NFKD", texto)
    texto = "".join(c for c in texto if not unicodedata.combining(c))
    return " ".join(texto.split())


def carregar_sinonimos() -> dict[str, str]:
    """expressao usada pelo paciente -> sintoma canonico da ontologia."""
    tabela: dict[str, str] = {}
    with SINONIMOS.open(encoding="utf-8") as fh:
        for linha in csv.DictReader(fh):
            tabela[normalizar(linha["expressao_no_relato"])] = normalizar(
                linha["sintoma_canonico"]
            )
    return tabela


def carregar_mapa() -> list[dict]:
    regras = []
    with MAPA.open(encoding="utf-8") as fh:
        for linha in csv.DictReader(fh):
            regras.append({
                "sintoma_1": normalizar(linha["sintoma_1"]),
                "sintoma_2": normalizar(linha["sintoma_2"]),
                "doenca": linha["doenca_associada"].strip(),
                "peso": float(linha["peso"]),
                "urgencia": linha["nivel_urgencia"].strip(),
            })
    return regras


def extrair_sintomas(relato: str, sinonimos: dict[str, str]) -> list[tuple[str, str]]:
    """Devolve [(expressao encontrada, sintoma canonico)], sem repetir canonicos.

    Expressoes mais longas sao testadas primeiro: 'falta de ar para deitar'
    deve vencer 'falta de ar' quando as duas casam no mesmo trecho.
    """
    texto = normalizar(relato)
    achados: list[tuple[str, str]] = []
    vistos: set[str] = set()
    for expressao in sorted(sinonimos, key=len, reverse=True):
        if expressao in texto:
            canonico = sinonimos[expressao]
            if canonico not in vistos:
                vistos.add(canonico)
                achados.append((expressao, canonico))
    return achados


def pontuar(sintomas: set[str], regras: list[dict]) -> list[dict]:
    """Soma os pesos das regras acionadas por doenca."""
    placar: dict[str, dict] = {}
    for regra in regras:
        tem_1 = regra["sintoma_1"] in sintomas
        tem_2 = regra["sintoma_2"] in sintomas
        if not (tem_1 or tem_2):
            continue
        completo = tem_1 and tem_2
        ganho = regra["peso"] * (PESO_COMPLETO if completo else PESO_PARCIAL)

        item = placar.setdefault(regra["doenca"], {
            "doenca": regra["doenca"],
            "pontos": 0.0,
            "regras_completas": 0,
            "evidencias": [],
            "urgencia": "rotina",
        })
        item["pontos"] += ganho
        if completo:
            item["regras_completas"] += 1
            item["evidencias"].append(f"{regra['sintoma_1']} + {regra['sintoma_2']}")

        # Em triagem, erra-se para o lado seguro: vale a MAIOR urgencia entre
        # todas as regras acionadas, inclusive as de casamento parcial.
        ordem = {"rotina": 0, "urgente": 1, "emergencia": 2}
        if ordem[regra["urgencia"]] > ordem[item["urgencia"]]:
            item["urgencia"] = regra["urgencia"]

    return sorted(
        placar.values(),
        key=lambda d: (d["regras_completas"], d["pontos"]),
        reverse=True,
    )


def analisar(relato: str, sinonimos: dict[str, str], regras: list[dict]) -> dict:
    achados = extrair_sintomas(relato, sinonimos)
    canonicos = {c for _, c in achados}
    ranking = pontuar(canonicos, regras)

    principal = ranking[0] if ranking else None
    confianca = "indefinida"
    if principal:
        if principal["regras_completas"] >= 2:
            confianca = "alta"
        elif principal["regras_completas"] == 1:
            confianca = "media"
        else:
            confianca = "baixa"
        # empate tecnico rebaixa a confianca
        if len(ranking) > 1 and abs(ranking[0]["pontos"] - ranking[1]["pontos"]) < 0.01:
            confianca = "baixa (empate entre hipoteses)"

    return {
        "relato": relato.strip(),
        "sintomas": achados,
        "ranking": ranking,
        "principal": principal,
        "confianca": confianca,
    }


def imprimir(idx: int, resultado: dict) -> None:
    print(f"\n{'=' * 78}")
    print(f"PACIENTE {idx:02d}")
    print(f"{'=' * 78}")
    print(f'Relato: "{resultado["relato"]}"')

    if not resultado["sintomas"]:
        print("\nNenhum sintoma reconhecido pela ontologia.")
        print("  -> relato encaminhado para triagem humana")
        return

    print("\nSintomas extraidos:")
    for expressao, canonico in resultado["sintomas"]:
        seta = "" if expressao == canonico else f'   (do trecho: "{expressao}")'
        print(f"  - {canonico}{seta}")

    principal = resultado["principal"]
    if not principal:
        print("\nNenhum diagnostico associado. Encaminhar para triagem humana.")
        return

    print(f"\nHipotese principal: {principal['doenca']}")
    print(f"  Pontuacao ....... {principal['pontos']:.1f}")
    print(f"  Confianca ....... {resultado['confianca']}")
    print(f"  Conduta ......... {URGENCIA_TEXTO[principal['urgencia']]}")
    if principal["evidencias"]:
        print("  Evidencia ....... " + "; ".join(principal["evidencias"]))

    outras = resultado["ranking"][1:4]
    if outras:
        print("\n  Diagnosticos diferenciais:")
        for alt in outras:
            print(f"    - {alt['doenca']:<34} {alt['pontos']:>5.1f} pts")


def main() -> int:
    ap = argparse.ArgumentParser(description="Extrator de sintomas do CardioIA")
    ap.add_argument("--frase", help="analisa uma unica frase informada")
    ap.add_argument("--csv", type=Path, help="salva o resultado em um CSV")
    args = ap.parse_args()

    for caminho in (FRASES, MAPA, SINONIMOS):
        if not caminho.exists():
            sys.exit(f"Arquivo nao encontrado: {caminho}")

    sinonimos = carregar_sinonimos()
    regras = carregar_mapa()

    print("CardioIA - Fase 2 | Parte 1: extracao de sintomas e apoio ao diagnostico")
    print(f"Ontologia: {len(regras)} regras | {len(sinonimos)} expressoes mapeadas")

    if args.frase:
        relatos = [args.frase]
    else:
        relatos = [l for l in FRASES.read_text(encoding="utf-8").splitlines() if l.strip()]
        print(f"Relatos:   {len(relatos)} frases em {FRASES.name}")

    resultados = [analisar(r, sinonimos, regras) for r in relatos]
    for i, res in enumerate(resultados, 1):
        imprimir(i, res)

    # --- resumo ------------------------------------------------------------
    print(f"\n{'=' * 78}")
    print("RESUMO DA TRIAGEM")
    print(f"{'=' * 78}")
    por_urgencia: dict[str, int] = {}
    sem_diagnostico = 0
    for res in resultados:
        if res["principal"]:
            u = res["principal"]["urgencia"]
            por_urgencia[u] = por_urgencia.get(u, 0) + 1
        else:
            sem_diagnostico += 1
    for nivel in ("emergencia", "urgente", "rotina"):
        if nivel in por_urgencia:
            print(f"  {nivel:>12}: {por_urgencia[nivel]} paciente(s)")
    if sem_diagnostico:
        print(f"  {'sem hipotese':>12}: {sem_diagnostico} paciente(s)")

    cobertura = sum(1 for r in resultados if r["sintomas"]) / len(resultados)
    print(f"\n  Cobertura da ontologia: {cobertura:.0%} dos relatos "
          f"tiveram ao menos um sintoma reconhecido")

    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["id", "relato", "sintomas_extraidos",
                        "diagnostico_sugerido", "pontuacao", "confianca",
                        "urgencia", "diferenciais"])
            for i, res in enumerate(resultados, 1):
                p = res["principal"]
                w.writerow([
                    f"R{i:02d}",
                    res["relato"],
                    "; ".join(c for _, c in res["sintomas"]),
                    p["doenca"] if p else "",
                    f"{p['pontos']:.1f}" if p else "",
                    res["confianca"],
                    p["urgencia"] if p else "",
                    "; ".join(a["doenca"] for a in res["ranking"][1:4]),
                ])
        print(f"\n  Resultado salvo em {args.csv}")

    print("\nAVISO: ferramenta academica de apoio. Nao substitui avaliacao medica.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
