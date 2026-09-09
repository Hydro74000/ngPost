# Publication sécurisée

Les workflows disposent de `contents: read` par défaut. Seul le job de
publication possède les droits d’écriture de release et d’attestation ; il
dépend des suites unitaires, d’intégration, GUI et VPN. Les actions externes
sont épinglées à des commits complets. Les dépendances téléchargées par les
scripts de compilation ne sont pas toutes épinglées : cela reste un chantier
distinct de reproductibilité de la chaîne de compilation.

L’environnement GitHub `release-signing` doit être configuré avec les secrets :

- `RELEASE_SIGNING_KEY` : clé privée PEM RSA (3072 bits minimum), correspondant
  à la clé publique à provisionner dans `src/utils/update/update-key.pem` ;
- `WINDOWS_SIGNING_PFX` (PFX encodé base64), `WINDOWS_SIGNING_PASSWORD` ;
- `MACOS_SIGNING_P12` (base64), `MACOS_SIGNING_PASSWORD`, `MACOS_SIGNING_IDENTITY`,
  `APPLE_ID`, `APPLE_APP_PASSWORD`, `APPLE_TEAM_ID`.

Protéger cet environnement et les tags de release, avec approbation humaine
et restrictions de branches adaptées au dépôt. Aucun secret ni certificat de
production n’est fourni par le code. Sans eux, la publication échoue : elle
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

La configuration effective des protections GitHub et une publication réelle
sur les trois plateformes restent à valider par les mainteneurs munis des
identités de signature. Le VPN reste exclu de macOS.
