# Validation des six correctifs — 10 septembre 2026

**Décision ultérieure du mainteneur :** les signatures et certificats ont été
retirés au profit de hashes SHA-256 publiés sur GitHub. Les observations de
signature/provisionnement ci-dessous sont historiques, pas la politique
actuelle. La provenance keyless GitHub a ensuite été rétablie, sans certificat
payant ni secret du mainteneur. Voir `release-security.md` et `checksum-updates.md`. La sauvegarde
privée locale a été préservée et n'est plus utilisée par l'updater ou la CI.

Six commits locaux, dans l’ordre demandé : P0 migration du helper, SEC-04,
SEC-05, SUP-01, VPN-02, VPN-03. Aucun push ni publication effectué.

**Suivi du 10 septembre :** ces commits ont depuis été poussés sur `devel`,
avec le provisionnement de la clé de signature des mises à jour. Le test de
correspondance du secret GitHub et de la clé publique a réussi en CI ; voir
`release-security.md`. Les résultats ci-dessous restent le relevé de la
validation locale initiale. Les certificats officiels Windows/Apple restent
à fournir ; aucune release n’a été publiée par cette opération.

## Vérifications exécutées

Les compilations ont été séquentielles, exclusivement avec `make -j1` dans
`ngpost-qt6-test`, en conservant les objets déjà compilés. Les sources modifiées
ont été comparées avec celles du conteneur. Aucune VM Windows/macOS démarrée.

| Vérification | Résultat |
| --- | --- |
| Application Qt 6 Linux, GUI compilée, lancement `--version` | Réussi, `SSL support: yes` |
| `tst_VpnProfile` | 37 succès |
| `tst_PathHelper` | 45 succès |
| `tst_UpdateChecker` | 12 succès |
| `tst_WireGuardBackend` | 13 succès |
| `tst_RandomToken` | 3 succès |
| `tst_WindowsCommandLine` | 3 succès, 2 cas natifs Windows ignorés |
| `tst_WindowsServiceControl` | 5 succès, 1 cas natif Windows ignoré |
| Tests Python de signature, archives et transactions | 11 succès |
| Migration privilégiée dans un conteneur jetable, réseau coupé, RAM 256 Mio | Réussie : échec fermé puis installation valide |
| YAML, permissions des workflows, SHA des actions, dépendances de publication | Contrôles structurels réussis |
| `bash -n` des scripts Linux modifiés, `git diff --check` | Réussis |

Les tests couvrent notamment l’annulation suivie d’une nouvelle tentative,
les callbacks tardifs, la survie de la transaction après fermeture normale,
les signatures/digests altérés, les chemins malveillants, les limites mémoire
des métadonnées et de l’extraction, le rollback Linux réel, les branches
Windows/macOS simulées et le maintien de l’état d’arrêt malgré des événements
tardifs. Les compteurs Qt incluent les routines d’initialisation/nettoyage.

## Limites et opérations encore nécessaires au déploiement

- **P0 :** le helper ancien est refusé par l’application. Sa neutralisation
  système nécessite l’authentification administrateur. Annuler cette
  authentification laisse l’ancien helper directement exploitable ; remplacer
  seulement l’AppImage ne peut pas révoquer une règle Polkit appartenant à root.
  La migration testée révoque l’ancien exécutable et la règle avant toute
  validation des sources, puis rétablit l’autorisation après remplacement.
- **SEC-05 / SUP-01 :** provisionner la clé publique de release et les secrets
  de signature/certificats décrits dans `release-security.md`. En leur absence,
  installation automatique et publication signée échouent volontairement.
  L’updater nécessite Python 3.9+ et OpenSSL. Le remplacement Windows est
  journalisé avec rollback, mais pas atomique face à une coupure électrique.
- **Windows :** compilation native, UAC, SID avec élévation sous un autre
  compte, droits SCM retirés et cycle réel import/démarrage/arrêt restent à
  valider sur Windows. Les tests Win32/PowerShell et le contrôle syntaxique des
  scripts sont inclus dans la CI, mais n’ont pas été exécutés localement.
- **macOS :** signature, notarisation et échange de bundle restent à tester
  avec les identités Apple sur macOS. Aucun support VPN macOS n’a été ajouté.
- Les suites complètes d’intégration, GUI et VPN E2E n’ont pas été lancées
  localement ; elles sont désormais des prérequis du job de publication.

Les fichiers utilisateur non suivis ont été préservés. Aucun helper, règle
Polkit, service VPN ni certificat n’a été installé ou modifié sur l’hôte.
