# Publications sans certificats — vérification SHA-256

À la demande du mainteneur, ngPost ne signe plus ses propres exécutables,
installateurs ou manifestes. Aucune identité Windows/Apple, aucun secret de
signature ni approbation de l'environnement release-signing n'est nécessaire
aux builds et publications. La notarisation Apple reste désactivée. Une
attestation de provenance keyless GitHub est produite séparément, sans clé
ni certificat à fournir par le mainteneur. Les signatures des outils tiers distribués par
leurs fournisseurs ne sont pas modifiées.

## Ce que contient chaque nouvelle release

- Les paquets Linux, Windows, macOS, les setups et les fichiers AppImage/zsync.
- Un SBOM SPDX décrivant les composants détectés.
- SHA256SUMS : une ligne SHA-256 par artefact.
- manifest.json : tag, nom, taille et SHA-256 de chaque artefact, pour l'updater.
- Les mêmes hashes, visibles directement dans le texte de la release GitHub.

Le générateur .github/scripts/release-checksums.py calcule les hashes sur les
fichiers définitifs, après compilation et packaging. La CI les revérifie avant
publication. Les manifestes ne sont pas signés et aucun fichier .sig n'est
produit. Les releases déjà publiées ne sont pas modifiées rétroactivement.

## Ce que vérifie ngPost

L'updater récupère le manifeste et le paquet depuis la même release GitHub
via HTTPS et une liste stricte d'hôtes autorisés. Il refuse toute installation
si le manifeste manque, si le tag ou le nom ne correspond pas, si la taille
est incorrecte ou si le SHA-256 calculé sur le fichier téléchargé diffère.

Ce contrôle vérifie l'intégrité par rapport au hash publié. Il ne prouve pas
indépendamment l'identité de l'éditeur : si le compte GitHub est compromis,
un attaquant pourrait remplacer à la fois le fichier et son hash. Ce choix
est explicite, sans prétendre offrir la garantie d'une signature numérique.

Les systèmes peuvent afficher des avertissements pour les logiciels non
signés. Le contrôle SHA-256 de ngPost ne remplace pas la confiance de Windows
ou de Gatekeeper.

## Vérification manuelle

Télécharger le paquet et SHA256SUMS depuis la même release. Sous Linux :

    sha256sum ngPost-<version>-linux-x86_64.tar.gz

Sous macOS :

    shasum -a 256 ngPost-<version>-macos.zip

Sous PowerShell :

    Get-FileHash .\ngPost-<version>-windows-x86_64.zip -Algorithm SHA256

Comparer le résultat à la ligne du même fichier dans SHA256SUMS.
Avec tous les artefacts présents dans le même dossier, Linux permet aussi :

    sha256sum --check SHA256SUMS

## Provenance keyless, distincte des signatures Windows/Apple

Après les hashes, la CI atteste les paquets définitifs, le SBOM, `SHA256SUMS`
et `manifest.json` via GitHub OIDC et Sigstore. Le certificat éphémère est
automatiquement délivré : aucun achat, secret de signature ou environnement
protégé à configurer. Un échec d'attestation bloque la publication.

Pour vérifier un fichier téléchargé et l'identité du workflow :

    gh attestation verify FICHIER -R Hydro74000/ngPost --signer-workflow Hydro74000/ngPost/.github/workflows/release.yml

Comparer aussi le commit source à celui attendu ; `gh attestation verify`
permet de l'imposer avec `--source-digest SHA_COMPLET`. Cette valeur doit
provenir d'une référence de confiance, pas uniquement de la release à vérifier.
L'attestation lie un digest à une exécution du workflow ; elle ne garantit pas
l'innocuité du code ni la sécurité d'un workflow compromis. L'updater intégré
ne vérifie pas encore ces attestations : son contrôle obligatoire reste SHA-256.

Références : [GitHub Actions](https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations)
et [vérification CLI](https://cli.github.com/manual/gh_attestation_verify).

## Protections CI conservées

Permissions contents: read par défaut, contents: write uniquement pour le
job de publication, qui possède aussi `id-token: write` et `attestations: write`
pour la provenance keyless ; actions épinglées à un SHA complet ; publication
conditionnée aux suites unitaires, intégration, GUI et VPN. Les secrets de
signature ne sont plus référencés et aucun environnement protégé ne bloque
la publication. Les dépendances des scripts de compilation ne sont pas toutes
épinglées ; le SBOM ne garantit pas l'inventaire complet des bibliothèques
statiquement liées.

La clé privée précédemment créée reste conservée localement hors Git ainsi
que dans le secret GitHub existant, désormais inutilisé. Elle n'a pas été
détruite. La clé publique et les scripts de signature retirés restent
récupérables dans l'historique Git.

Le VPN reste exclu de macOS. Voir checksum-updates.md pour le contrat technique.

## Validation du retrait des signatures

- Build Qt 6 Linux et lancement `--version` réussis, compilation séquentielle `make -j1`.
- `tst_UpdateChecker` : 13 succès, aucune erreur.
- Tests Python : 14 succès ; le test privilégié de migration Linux est ignoré
  hors conteneur jetable explicitement autorisé.
- Génération des hashes sans secret, concordance fichier/manifeste,
  reproductibilité, rejet des signatures résiduelles et liens symboliques testés.
- Hash absent, mal formé ou différent : installation refusée avant extraction.
- Les six workflows YAML sont syntaxiquement valides. Les builds natifs
  Windows/macOS et la publication finale restent à confirmer par la nouvelle CI.
