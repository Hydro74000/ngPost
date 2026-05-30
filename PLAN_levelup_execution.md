# LevelUp execution tracking

Branch: `levelup` tracking `origin/levelup`
Commit valide CI: `5771094`

## En cours

_Aucun bloc en cours._

## A faire

_Aucun bloc restant._

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
- [x] Correctif SQLite: requetes `markArticlePosted`, `markArticleFailed`, `markArticleUnknown`
- [x] Correctif CLI help: alias longs underscore affiches avec `--`
- [x] Robustesse mock NNTP Windows/CI: timeout de demarrage configurable, 15s par defaut
- [x] Workflows GitHub separes: build, unit tests, integration tests, gui tests, vpn e2e
- [x] Validation Windows VM: build app OK, unit tests OK, integration tests OK
- [x] Validation locale ciblee: `tst_PostHistory` OK dans `my-distrobox`
- [x] Validation syntaxe: `git diff --check` OK, workflows YAML parses via `js-yaml`
- [x] Commit validation/CI associe: `a233b7d` (`Fix LevelUp validation and CI workflows`)
- [x] Push effectue vers `origin/levelup`
- [x] GitHub Actions premiere passe: GUI et VPN OK, build/unit/integration KO sur QtKeychain Windows
- [x] GitHub Actions corrigees: build, unit tests, integration tests, GUI, VPN E2E OK sur `cc69baa`
- [x] Diagnostic GitHub Actions: logs QtTest explicites et artefacts `.log`/`.stdout.log`
- [x] GitHub Actions finales: build, unit tests, integration tests, GUI, VPN E2E OK sur `5771094`

## Journal

- Initialisation: branche creee et publiee. `.gitignore` est inclus dans le perimetre a la demande utilisateur.
- 2026-05-19T15:54:18+02:00: onglet Historique isole des onglets de post rapides pour eviter les casts invalides et fermetures accidentelles.
- 2026-05-19T16:05:19+02:00: `git diff --check` OK; build `make -C src -j2` et tests `make -C tests -j2` bloques car `/usr/bin/qmake6` est absent.
- 2026-05-19T16:05:19+02:00: commit local cree: `a0eba5f`.
- 2026-05-19T17:10:00+02:00: correction du mismatch SQLite: `tst_PostHistory` passe 5/5 sous Linux et Windows.
- 2026-05-19T17:35:00+02:00: build Windows propre dans `C:\ngPost_ci` / `C:\ngPost_build_ci`; `ngPost.exe` compile sans erreur.
- 2026-05-19T17:50:00+02:00: unit tests Windows OK: `tst_CliParser` 8/8, `tst_PostHistory` 5/5, `tst_PathHelper` 10/10, `tst_VpnProfile` 11/11, `tst_WireGuardBackend` 13/13, `tst_WindowsBindHelper` 9/9, `tst_Yenc` 7/7.
- 2026-05-19T18:20:00+02:00: integration tests Windows OK apres timeout mock NNTP: `tst_GoldenNzb` 4/4, `tst_MockNntp` 8/8, `tst_PostFlow` 7/7, `tst_VpnDnsResolver` 8/8.
- 2026-05-19T18:25:00+02:00: build Linux complet dans `my-distrobox` encore bloque par dependance locale manquante `QtCharts` (`Unknown module(s) in QT: charts`); les workflows installent `qtcharts`.
- 2026-05-19T18:35:00+02:00: branche `levelup` poussee sur `origin/levelup`.
- 2026-05-19T18:50:00+02:00: logs GitHub Actions analyses; les echecs Windows viennent de QtKeychain `messages`/traductions (`Error -1073741515`) avant build ngPost.
- 2026-05-19T19:00:00+02:00: deuxieme blocage GitHub Actions: QtKeychain Windows compile avec l'option MSVC `/utf-8`; patch workflow ajoute une substitution vers flags GCC UTF-8.
- 2026-05-19T19:15:00+02:00: dernier commit CI fonctionnel `cc69baa`; workflows GitHub OK: Development Project Build, Unit Tests, Integration Tests, GUI Tests, VPN E2E.
- 2026-05-19T19:35:00+02:00: nouvelle passe GitHub Actions sur `d130b4d`; integration Windows KO sans artefact ni sortie QtTest detaillee, correction du logging CI en cours.
- 2026-05-19T19:45:00+02:00: script commun `.github/scripts/run-qt-tests.sh` ajoute; integration locale Linux OK et logs QtTest visibles.
- 2026-05-19T20:05:00+02:00: workflows GitHub OK sur `5771094`: Development Project Build, Unit Tests, Integration Tests, GUI Tests, VPN E2E.
