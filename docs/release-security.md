# Publication sécurisée

Les workflows disposent de `contents: read` par défaut. Seul le job de
publication possède les droits d’écriture de release et d’attestation ; il
dépend des suites unitaires, d’intégration, GUI et VPN. Les actions externes
sont épinglées à des commits complets. Les dépendances téléchargées par les
scripts de compilation ne sont pas toutes épinglées : cela reste un chantier
distinct de reproductibilité de la chaîne de compilation.

## État de configuration

La clé publique RSA 4096 bits est maintenant embarquée dans les ressources Qt
communes aux builds Linux, Windows et macOS (y compris les exécutables fournis
par les setups). Sa partie privée est stockée dans le secret d’environnement
GitHub `RELEASE_SIGNING_KEY`, jamais dans le dépôt ou les artefacts.

Empreinte SHA-256 de la clé publique (SPKI DER) :

```text
bd849b083dd61a9598515c09fdd579e3b8ca7d9620ff5073ac06d1fe717c9811
```

L’environnement `release-signing` est limité aux branches `devel` et `master`
et demande l’approbation de `Hydro74000`. Le workflow **Release signing key
check** vérifie la correspondance secret/clé publique sans créer de release.
Son approbation ne vaut pas approbation du workflow **Build and Release**.
La première [vérification réelle en CI](https://github.com/Hydro74000/ngPost/actions/runs/34456555994)
a réussi le 10 septembre 2026 : signature d’une preuve non publiable avec le
secret GitHub, puis vérification avec la clé publique embarquée.
Les exécutions manuelles avec `publish: false` utilisent `build-only`, sans
secret de production ni certificat de plateforme.

Les certificats officiels Windows/Apple ne sont pas créés par cette opération.
Tant qu’ils manquent, ne pas approuver une publication de production : les
étapes de signature refuseront de publier sans ces identités. Les builds et
tests ordinaires continuent sans elles.

## Secrets attendus

L’environnement GitHub `release-signing` utilise les secrets :

- `RELEASE_SIGNING_KEY` : clé privée PEM RSA (3072 bits minimum), correspondant
  à la clé publique dans `src/utils/update/update-key.pem` (déjà configuré) ;
- `WINDOWS_SIGNING_PFX` (PFX encodé base64), `WINDOWS_SIGNING_PASSWORD` ;
- `MACOS_SIGNING_P12` (base64), `MACOS_SIGNING_PASSWORD`, `MACOS_SIGNING_IDENTITY`,
  `APPLE_ID`, `APPLE_APP_PASSWORD`, `APPLE_TEAM_ID`.

Conserver les protections de l’environnement et protéger les tags de release
selon les règles du dépôt. Aucun secret ni certificat privé de production
n’est fourni par le code. Sans les identités requises, la publication échoue : elle
ne retombe pas sur une publication non signée. Une exécution manuelle avec
`publish: false` permet la compilation sans signature de plateforme.

Les exécutables et installateurs Windows sont signés et vérifiés avec
Authenticode. Le bundle macOS est signé, soumis à notarisation puis agrafé.
Le job final produit un SBOM SPDX des artefacts, signe `SHA256SUMS` et le
`manifest.json` consommé par l’updater, puis atteste les fichiers publiés.
Ce SBOM décrit ce que l’analyse des paquets binaires détecte, pas une garantie
d’inventaire exhaustif des dépendances statiquement liées. L’attestation
identifie le workflow de publication, sans rendre les builds reproductibles.

Le ZIP macOS utilise `ditto --keepParent` pour préserver le bundle signé,
conformément au [guide de distribution Apple](https://developer.apple.com/documentation/xcode/packaging-mac-software-for-distribution).

Une publication réelle sur les trois plateformes reste à valider par les
mainteneurs munis des identités de signature. Le VPN reste exclu de macOS.

## Sauvegarde et rotation

Une copie locale de la clé privée a été conservée hors du dépôt, dans un
répertoire accessible uniquement à son propriétaire. La sauvegarder dans un
coffre chiffré : GitHub ne permet pas de relire la valeur d’un secret.
Ne pas recréer une paire pour chaque build et ne pas remplacer la clé publique
seule après déploiement. Une rotation nécessite une transition de confiance
explicitement conçue et validée pour les clients déjà distribués.

Pour vérifier uniquement la partie publique :

```sh
python3 .github/scripts/check-release-key.py
```
