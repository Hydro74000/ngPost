# LevelUp execution tracking

Branch: `levelup` tracking `origin/levelup`
Commit: `a0eba5f`

## En cours

- [ ] Validation finale et push

## A faire

- [ ] Revalidation build/tests dans un environnement avec `/usr/bin/qmake6`

## Termine

- [x] Preflight git: `git fetch origin`
- [x] Branche `levelup` creee depuis `devel`
- [x] Remote tracking cree: `origin/levelup`
- [x] Stockage SQLite commun: schema v1, migrations, transactions, config `POST_DB`, `HISTORY_STORE_PASSWORDS`
- [x] Hooks posting/fichiers/articles/tentatives: creation post/fichiers/articles, tentatives `posting`, `posted`, `failed`, `unknown`
- [x] Regeneration NZB depuis historique: sortie stdout/fichier, masquage password par defaut
- [x] CLI historique/import/regeneration/reprise: commandes et alias tiret/underscore ajoutes
- [x] IHM Historique/Stats/Reprise: onglet, banniere de reprise, tables et graphique initial
- [x] Stabilisation build statique: signatures et diff whitespace verifies
- [x] Documentation README/config/help: README EN/FR et notes de version mises a jour
- [x] Tests unitaires/integration/gui: tests unitaires historique ajoutes, test help CLI etendu
- [x] Commit associe: `a0eba5f` (`Implement LevelUp history and resume`)

## Journal

- Initialisation: branche creee et publiee. `.gitignore` est inclus dans le perimetre a la demande utilisateur.
- 2026-05-19T15:54:18+02:00: onglet Historique isole des onglets de post rapides pour eviter les casts invalides et fermetures accidentelles.
- 2026-05-19T16:05:19+02:00: `git diff --check` OK; build `make -C src -j2` et tests `make -C tests -j2` bloques car `/usr/bin/qmake6` est absent.
- 2026-05-19T16:05:19+02:00: commit local cree: `a0eba5f`.
