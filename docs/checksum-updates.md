# Contrat de mise à jour SHA-256

## Prérequis et garanties

La mise à jour intégrée nécessite Python 3.9+ côté client et le marqueur de
paquet `.ngpost-installation`. Leur absence provoque un refus avant modification.
Les répertoires système Linux et les installations gérées par un gestionnaire de
paquets utilisent une mise à jour manuelle. AppImage conserve son chemin séparé.

Aucun certificat, clé ou exécutable OpenSSL n'est requis par l'updater. Qt utilise
toujours son moteur TLS pour HTTPS : la sécurité du transport reste activée.

Chaque release fournit `manifest.json` et `SHA256SUMS`. Le manifeste JSON UTF-8
contient `schema: 1`, `tag` et `assets`, un tableau de `{name, size, sha256}`.
L'updater vérifie le tag, le nom exact du paquet, sa taille et son SHA-256 avant
extraction. Un hash mal formé, une métadonnée manquante, des champs JSON dupliqués
ou plusieurs entrées pour le paquet demandé provoquent un refus.
`SHA256SUMS` contient les mêmes hashes pour une vérification manuelle ; son
contenu est aussi reproduit dans la description de la release GitHub.

Les hashes vérifient l'intégrité par rapport aux métadonnées GitHub, pas
indépendamment l'identité de l'éditeur. Remplacer simultanément un paquet et
son manifeste permet de contourner ce contrôle. Aucun fichier de signature
`.sig`, certificat Windows/Apple ou secret de signature du mainteneur n'est requis.

Une attestation GitHub **keyless** est produite séparément pour les paquets et
manifestes. Elle est vérifiable avec `gh attestation verify`, mais **n'est pas
vérifiée automatiquement par l'updater actuel**. Elle lie les digests au workflow
et à sa source, sans garantir l'innocuité du code ou d'un workflow compromis.
Voir [la politique de publication](release-security.md) pour les commandes et
les restrictions d'identité du workflow et de commit à vérifier.

## Téléchargement et extraction

Les téléchargements utilisent HTTPS, une liste exacte d'hôtes GitHub autorisés,
des redirections validées, un délai de transfert de 30 secondes et des limites
de taille pour les métadonnées et les paquets. Les données sont écrites en flux
dans un répertoire privé aléatoire voisin de l'installation.

L'extraction refuse les traversées de chemins, noms Windows ambigus, doublons,
périphériques et liens physiques. Les liens symboliques internes sont créés
en dernier ; leur résolution complète doit rester dans le répertoire extrait.
La taille décompressée et le nombre d'entrées sont bornés. Les métadonnées du
répertoire central ZIP sont limitées avant allocation par `ZipFile` ; les en-têtes
étendus TAR sont limités avant traitement par `tarfile`. ZIP64 et les métadonnées
GNU sparse nécessitant des allocations supplémentaires ne sont pas pris en charge.
L'interception du traitement `tarfile` est couverte par des tests et doit être
revérifiée lors des changements de version de Python.

## Remplacement et récupération

Après validation du hash, le candidat doit réussir `--version` avant la fermeture
de l'application. Linux/macOS échangent les répertoires atomiquement via
`renameat2`/`renamex_np` ; un système de fichiers incompatible provoque un refus
sans modifier l'installation. Windows utilise deux renommages journalisés après
fermeture : c'est récupérable, **pas un échange atomique unique**. Un échec du
second renommage restaure immédiatement l'ancien répertoire. Un échec du test
ou du lancement après permutation déclenche également un retour arrière.
Une coupure électrique entre les renommages Windows nécessite une récupération
à partir du journal conservé.

Le répertoire privé `.ngpost-update-*` conserve `transaction.json`, l'ancienne
installation (`previous` sous Windows, chemin `candidate` du journal sous POSIX)
et `error.txt` en cas d'échec. L'updater ne supprime jamais récursivement une
installation. Pour récupérer manuellement : fermer ngPost, conserver ailleurs
la version défaillante et renommer l'ancienne vers le chemin `install` du journal.

L'installateur attend la fermeture du processus parent. « Annuler » interrompt
le téléchargement/la préparation et écrit un marqueur contrôlé avant permutation.
Les callbacks tardifs d'une tentative annulée ne peuvent pas affecter un nouvel
essai. La fermeture programmée de la progression et la destruction normale de
l'application après transfert à l'installateur n'annulent pas la transaction.

Les chemins Windows/macOS doivent être validés nativement en CI. Aucun certificat
de plateforme ni approbation de signature n'est nécessaire à la publication.
Le VPN reste exclu de macOS.
