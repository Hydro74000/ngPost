# Plan fonctionnel Level Up

Ce document cadre les evolutions souhaitees autour de l'historique, des statistiques,
de la regeneration de NZB et de la reprise de post. Il reste volontairement
fonctionnel : le plan d'implementation technique sera defini ensuite.

## Vision

Faire evoluer ngPost d'un outil qui poste et produit un NZB vers un outil capable
de garder une memoire exploitable de ce qui a ete poste.

Cette memoire doit permettre de :

- consulter l'historique des posts depuis l'IHM ;
- rechercher et filtrer les posts ;
- produire des statistiques utiles ;
- rendre l'ecriture de l'historique robuste ;
- regenerer un NZB sans retrouver le fichier NZB original ;
- gerer proprement les mots de passe d'archive ;
- preparer une reprise fiable apres pause, echec, coupure reseau ou crash.

## Objectifs fonctionnels

### Historique structure

Chaque post doit etre historise avec assez d'informations pour etre compris,
recherche, exporte et reutilise.

Informations attendues :

- date de creation du post ;
- date de debut d'upload ;
- date de fin ;
- nom du post ;
- nom du NZB ;
- chemin du NZB genere, s'il existe encore ;
- statut final ;
- taille totale postee ;
- vitesse moyenne ;
- nombre de fichiers ;
- nombre total d'articles ;
- nombre d'articles echoues ;
- groupes utilises ;
- poster/from ;
- nom d'archive ;
- mot de passe d'archive ;
- origine du mot de passe : absent, fourni, fixe, genere ou non historise ;
- indicateur de presence d'un mot de passe meme si sa valeur n'est pas stockee ;
- options importantes du post, notamment obfuscation, compression et par2.

Statuts fonctionnels proposes :

- `posting` : post en cours ;
- `success` : post termine sans article echoue ;
- `partial` : post termine mais incomplet, avec des fichiers ou articles
  echoues ;
- `failed` : post interrompu par erreur bloquante ;
- `cancelled` : post arrete volontairement ;
- `resumable` : post incomplet pour lequel ngPost dispose des informations
  necessaires a une reprise ;
- `unknown` : etat incertain apres crash ou interruption brutale.

Regle fonctionnelle :

- un post `resumable` doit etre propose a la reprise ;
- un post `partial` doit aussi etre propose a la reprise quand les informations
  necessaires existent ;
- un post `partial` peut rester non reprenable si l'historique ne permet pas
  d'identifier proprement les fichiers ou articles manquants ;
- la distinction est donc volontaire : `partial` decrit le resultat du post,
  tandis que `resumable` decrit sa capacite de reprise.

### Gestion des mots de passe d'archive

Le mot de passe d'archive doit etre une donnee fonctionnelle explicite, car il
est necessaire pour certains usages mais sensible.

Cas a gerer :

- post sans mot de passe ;
- mot de passe fourni manuellement ;
- mot de passe fixe configure par l'utilisateur ;
- mot de passe genere par ngPost ;
- mot de passe volontairement non historise.

Informations attendues :

- presence ou absence d'un mot de passe ;
- valeur du mot de passe si l'utilisateur autorise son stockage ;
- origine du mot de passe ;
- date de creation ou d'association au post ;
- indicateur signalant que le mot de passe a ete masque, supprime ou non stocke.

Comportement attendu :

- masquer les mots de passe par defaut dans l'IHM ;
- proposer une action explicite pour reveler un mot de passe ;
- proposer une action pour copier le mot de passe sans l'afficher durablement ;
- ne pas exposer les mots de passe dans les exports par defaut ;
- permettre un export explicite incluant les mots de passe ;
- permettre de purger uniquement les mots de passe sans supprimer l'historique ;
- permettre de desactiver l'historisation des mots de passe via configuration ;
- avertir clairement que l'historique est sensible si les mots de passe sont
  conserves.

Recherche et filtrage :

- filtrer par presence ou absence de mot de passe ;
- filtrer par origine du mot de passe ;
- eviter la recherche texte libre dans les mots de passe par defaut ;
- permettre une recherche explicite par mot de passe exact si cette option est
  jugee utile.

Regeneration de NZB :

- inclure le mot de passe dans les meta-informations du NZB regenere si la
  valeur est disponible et si l'utilisateur le demande ou l'autorise ;
- regenerer le NZB sans meta password si le mot de passe n'a pas ete historise ;
- avertir l'utilisateur quand un NZB est regenere sans mot de passe connu alors
  que le post indique qu'une archive etait protegee.

Reprise de post :

- reutiliser le meme nom d'archive et le meme mot de passe pour une reprise ;
- si le mot de passe genere n'a pas ete historise, signaler que certaines formes
  de reprise ou de regeneration d'archives peuvent etre impossibles ;
- ne jamais generer un nouveau mot de passe silencieusement pour la reprise d'un
  post deja commence.

### Historique par fichier et par article

Pour permettre la regeneration de NZB et la reprise fine, l'historique ne doit pas
se limiter au post global.

Pour chaque fichier poste, conserver :

- ordre dans le post ;
- chemin source d'origine quand il est connu ;
- nom expose dans le NZB ;
- nom obfusque si applicable ;
- taille ;
- nombre total d'articles ;
- groupes utilises pour ce fichier ;
- statut du fichier.

Pour chaque article, conserver :

- fichier parent ;
- numero de segment ;
- taille du segment ;
- position dans le fichier ;
- Message-ID final ;
- statut ;
- date de succes ou d'echec ;
- information minimale d'erreur si disponible.

Statuts article proposes :

- `pending` : pas encore envoye ;
- `posting` : en cours d'envoi ;
- `posted` : accepte par le serveur ;
- `failed` : refuse apres retry ;
- `unknown` : envoye mais resultat non confirme.

## Parcours utilisateur IHM

### Vue Historique

Ajouter une vue dediee dans l'IHM listant les posts connus.

La liste doit afficher au minimum :

- date ;
- nom ;
- statut ;
- taille ;
- vitesse moyenne ;
- groupes ;
- nombre d'echecs ;
- presence d'un mot de passe ;
- chemin du NZB si disponible.

La vue doit permettre :

- recherche texte sur nom, NZB, archive et groupes ;
- filtre par periode ;
- filtre par statut ;
- filtre par groupe ;
- filtre par presence ou origine du mot de passe ;
- filtre par presence d'erreurs ;
- tri par date, taille, vitesse, nom ou statut ;
- ouverture d'un panneau detail.

### Detail d'un post

Le panneau detail doit afficher :

- informations generales du post ;
- fichiers postes ;
- progression par fichier ;
- nombre d'articles par fichier ;
- articles echoues ou inconnus ;
- erreurs connues ;
- mot de passe d'archive si l'utilisateur choisit de l'afficher.

Actions disponibles :

- regenerer le NZB ;
- exporter la fiche du post ;
- exporter une selection en CSV ;
- copier le mot de passe si disponible ;
- purger le mot de passe de ce post ;
- reprendre un post resumable ;
- supprimer l'entree d'historique ;
- ouvrir l'emplacement du NZB existant, si disponible.

### Centre de reprise IHM

Au chargement de l'application, ngPost doit verifier s'il existe des posts
interrompus, `resumable` ou `partial` potentiellement reprenables.

Comportement attendu au demarrage :

- ne pas bloquer l'ouverture de l'application avec une grande modale ;
- afficher une banniere ou un indicateur clair si des posts peuvent etre repris ;
- proposer une action `Ouvrir la reprise` ;
- permettre d'ignorer l'alerte pour la session courante ;
- rendre l'alerte plus visible si un post etait actif lors d'un crash recent.

La vue de reprise doit permettre :

- lister les posts `resumable` ;
- lister les posts `partial` potentiellement reprenables ;
- afficher les posts `partial` non reprenables avec la raison ;
- selectionner un ou plusieurs posts a reprendre ;
- voir le detail avant reprise ;
- reprendre la selection ;
- ignorer temporairement la selection ;
- marquer la selection comme abandonnee ;
- supprimer les donnees de reprise avec confirmation ;
- localiser un dossier ou des fichiers sources manquants si necessaire.

La liste de reprise doit afficher au minimum :

- nom du post ;
- statut ;
- taille ;
- progression estimee ;
- nombre d'articles confirmes ;
- nombre d'articles a reposter ;
- nombre d'articles `unknown` ;
- raison bloquante si la reprise est impossible.

Regles UX :

- un post `resumable` peut etre coche par defaut ;
- un post `partial` reprenable doit etre visible, mais peut demander validation ;
- un post non reprenable doit etre desactive avec une raison lisible ;
- les libelles doivent distinguer `Ignorer`, `Abandonner` et `Supprimer les
  donnees de reprise` ;
- `Tout annuler` doit etre evite comme libelle, car il est ambigu.

### Statistiques

Ajouter une vue ou un onglet de statistiques base sur l'historique.

Statistiques attendues :

- volume poste par jour ;
- volume poste par semaine/mois ;
- vitesse moyenne par jour ;
- nombre de posts par jour ;
- nombre d'echecs par jour ;
- taux de succes ;
- repartition par groupe ;
- top des posts les plus volumineux ;
- evolution des articles echoues.

Les statistiques doivent pouvoir etre filtrees par periode et par groupe.

## Parcours utilisateur CLI

Prevoir des commandes CLI capables d'exploiter l'historique.

Fonctions attendues :

- lister les posts ;
- rechercher un post ;
- filtrer par date, statut ou groupe ;
- filtrer par presence de mot de passe ;
- afficher le detail d'un post ;
- regenerer un NZB vers stdout ;
- regenerer un NZB vers un fichier ;
- exporter l'historique en CSV, avec mots de passe masques par defaut ;
- lister les posts reprenables ;
- simuler une reprise sans la lancer ;
- reprendre un post `resumable` ;
- reprendre un post `partial` si ses donnees le permettent ;
- reprendre plusieurs posts ;
- abandonner un post incomplet ;
- purger les donnees de reprise ;
- produire une sortie lisible humainement ou une sortie structuree pour scripts.

Comportement CLI attendu :

- par defaut, une commande de post normale ne doit pas reprendre silencieusement
  un ancien post incomplet ;
- si des posts reprenables existent, la CLI peut afficher un rappel court avec
  la commande a utiliser ;
- toute commande destructive doit demander confirmation en mode interactif ;
- toute commande destructive doit exiger une option explicite en mode non
  interactif ;
- les commandes de reprise doivent fournir un mode dry-run ;
- les scripts doivent pouvoir utiliser une sortie JSON ou stable ;
- les codes de retour doivent distinguer succes, reprise impossible, confirmation
  requise et erreur technique.

Exemples fonctionnels non definitifs :

```bash
ngPost --history
ngPost --history --search "release-name"
ngPost --history --status partial
ngPost --history --has-password
ngPost --history-show <post-id>
ngPost --regenerate-nzb <post-id>
ngPost --regenerate-nzb <post-id> -o /path/post.nzb
ngPost --regenerate-nzb <post-id> --include-password
ngPost --resume-list
ngPost --resume-list --json
ngPost --resume-check <post-id>
ngPost --resume-post <post-id> --dry-run
ngPost --resume-post <post-id>
ngPost --resume-post <post-id> --yes
ngPost --resume-post <post-id-1>,<post-id-2>
ngPost --resume-all --dry-run
ngPost --resume-abandon <post-id>
ngPost --resume-purge <post-id> --yes
```

## Regeneration de NZB

La regeneration de NZB doit fonctionner meme si le fichier `.nzb` original a ete
deplace ou supprime.

Elle doit utiliser les donnees historisees :

- nom du post ;
- poster/from ;
- groupes ;
- fichiers ;
- segments ;
- tailles ;
- Message-ID finaux ;
- meta-informations, dont le mot de passe si applicable.

Comportement attendu :

- si le post est `success`, le NZB regenere doit etre complet ;
- si le post est `partial`, le NZB peut etre regenere avec avertissement ;
- si des articles sont `unknown`, l'utilisateur doit etre averti clairement ;
- si le mot de passe est connu, l'utilisateur doit pouvoir choisir s'il est
  inclus dans le NZB regenere ;
- si le mot de passe n'est pas connu, le NZB doit etre regenere avec
  avertissement et sans meta password ;
- si les donnees sont insuffisantes, la regeneration doit echouer proprement.

## Reprise de post

La reprise doit couvrir plusieurs niveaux.

### UX generale de reprise

La reprise doit etre visible et explicite, mais ne doit pas gener l'usage normal
de ngPost.

Comportement attendu :

- detecter les posts a reprendre au demarrage de l'IHM ;
- proposer une vue de reprise dediee dans l'IHM ;
- exposer les memes decisions principales en CLI ;
- ne jamais reprendre automatiquement un post ancien sans action explicite de
  l'utilisateur ;
- permettre une reprise automatique uniquement dans le cas d'un post deja actif
  qui subit une perte reseau temporaire ;
- separer clairement les actions `reprendre`, `ignorer`, `abandonner` et
  `purger` ;
- conserver les posts abandonnes dans l'historique, sauf si l'utilisateur demande
  explicitement leur suppression ;
- supprimer les donnees techniques de reprise uniquement apres confirmation.

### Pause / resume

La pause actuelle doit rester disponible.

Comportement attendu :

- suspendre les connexions ;
- reprendre sans perdre l'etat des articles deja acceptes ;
- conserver une progression coherent dans l'IHM ;
- ne pas marquer comme echoue ce qui est simplement mis en pause.

### Auto-resume sur perte reseau

En cas de perte reseau temporaire, ngPost doit pouvoir attendre puis retenter.

Le cas delicat concerne les articles en cours d'envoi au moment de la coupure :
ngPost peut avoir envoye le corps de l'article sans avoir recu la reponse NNTP
finale du serveur. Dans ce cas, il est impossible de savoir avec certitude si le
serveur a accepte l'article.

Comportement attendu :

- detecter la perte de toutes les connexions ;
- passer le post dans un etat de reprise automatique ;
- retenter apres un delai configurable ;
- conserver les articles deja confirmes ;
- marquer les articles en cours comme `unknown` si leur resultat est incertain.
- ne considerer un article comme `posted` qu'apres reception d'une confirmation
  serveur explicite ;
- ne pas inclure un article `unknown` non resolu dans un NZB final sans
  avertissement.

Regle de reprise envisagee :

- les articles `posted` sont conserves et ne sont jamais repostes ;
- les articles `pending` restent a poster normalement ;
- les articles `failed` peuvent etre repostes selon la politique de retry ;
- les articles `unknown` sont remis en file avec un nouveau Message-ID ;
- si le repost d'un article `unknown` reussit, le nouveau Message-ID devient le
  Message-ID officiel utilise dans l'historique et dans le NZB final ;
- l'ancien Message-ID incertain reste conserve comme trace technique, mais il
  n'est pas utilise dans le NZB final ;
- si l'ancien article avait en realite ete accepte par le serveur avant la
  coupure, il peut rester sur Usenet comme article non reference par le NZB ;
- ce doublon non reference est considere preferable a un NZB contenant un
  Message-ID incertain.

Comportement utilisateur :

- en auto-resume, ngPost ne doit pas demander de decision utilisateur pour chaque
  article `unknown` ;
- l'IHM et la CLI doivent afficher le nombre d'articles `unknown` detectes et le
  nombre d'articles repostes pour les resoudre ;
- si des articles `unknown` ne peuvent pas etre resolus apres retry, le post doit
  passer en `partial` ou `resumable` selon les donnees restantes ;
- le detail du post doit expliquer que les articles `unknown` correspondent a une
  coupure avant confirmation serveur.

Option avancee a evaluer plus tard :

- tenter de reutiliser le meme Message-ID pour verifier si le serveur signale un
  doublon deja accepte ;
- ne retenir cette strategie que si les reponses serveur peuvent etre interpretees
  de maniere suffisamment fiable ;
- pour une premiere version, privilegier le repost avec nouveau Message-ID.

### Resume apres echec

Quand un post echoue mais reste exploitable, l'utilisateur doit pouvoir le
reprendre plus tard.

Comportement attendu :

- relister les posts `resumable` et les posts `partial` potentiellement
  reprenables ;
- indiquer clairement dans l'IHM et la CLI si un post `partial` est reprenable,
  partiellement reprenable ou non reprenable ;
- expliquer la raison quand un post `partial` ne peut pas etre repris, par
  exemple donnees article manquantes, mot de passe absent ou fichiers sources
  introuvables ;
- reprendre uniquement les fichiers/articles manquants ;
- reutiliser les Message-ID deja confirmes ;
- reutiliser le mot de passe d'archive existant quand il est necessaire ;
- regenerer le NZB final a partir de l'historique consolide ;
- conserver une trace du nombre de reprises.

### Resume apres crash

Apres redemarrage de ngPost, les posts incomplets doivent etre detectables.

Comportement attendu :

- afficher les posts interrompus ;
- distinguer les articles `posted`, `pending`, `failed` et `unknown` ;
- proposer de reprendre ou d'abandonner ;
- transformer les articles `posting` restes en base en `unknown` ;
- reposter les articles incertains avec de nouveaux Message-ID quand necessaire.

## Robustesse d'ecriture

L'historique doit etre plus robuste qu'un CSV append-only.

Exigences fonctionnelles :

- ecriture transactionnelle ;
- pas de ligne partiellement ecrite ;
- coherence entre post, fichiers et articles ;
- possibilite de recuperer apres crash ;
- fonctionnement correct avec beaucoup d'articles ;
- pas de corruption si ngPost est ferme pendant un post ;
- export CSV possible pour compatibilite ou usage externe.

Le CSV historique existant peut rester comme export ou mode legacy, mais il ne
doit plus etre la source principale des fonctionnalites avancees.

## Confidentialite et donnees sensibles

L'historique peut contenir des mots de passe d'archive et des chemins locaux.

Exigences fonctionnelles :

- documenter clairement que l'historique est sensible ;
- masquer les mots de passe par defaut dans l'IHM ;
- demander une action explicite pour reveler un mot de passe ;
- permettre d'exclure les mots de passe de l'historique via configuration ;
- masquer les mots de passe dans les exports et sorties CLI par defaut ;
- permettre la purge globale ou selective des mots de passe historises ;
- permettre de purger l'historique ;
- permettre de supprimer un post specifique de l'historique.

## Compatibilite

Les utilisateurs existants ne doivent pas perdre leurs usages.

Comportement attendu :

- conserver `POST_HISTORY` tant que necessaire ;
- permettre l'export CSV depuis le nouvel historique ;
- ne pas rendre l'historique avance obligatoire pour poster ;
- prevoir une migration/import du CSV existant si utile ;
- ne pas casser les scripts bases sur les NZB existants.

## Documentation

Les nouvelles fonctionnalites doivent etre accompagnees d'une mise a jour des
documentations utilisateur et developpeur.

Documents a mettre a jour :

- `README.md` ;
- `README_FR.md` ;
- fichiers de configuration exemples ;
- aide CLI integree ;
- wiki utilisateur ;
- notes de version ;
- documentation de migration si un stockage historique structure remplace le CSV
  comme source principale.

Contenu attendu :

- expliquer le nouvel historique structure ;
- expliquer la difference entre historique, export CSV et donnees de reprise ;
- documenter les statuts `success`, `partial`, `resumable`, `failed`,
  `cancelled` et `unknown` ;
- documenter la vue Historique et le centre de reprise IHM ;
- documenter les commandes CLI d'historique ;
- documenter les commandes CLI de regeneration de NZB ;
- documenter les commandes CLI de reprise, dry-run, abandon et purge ;
- documenter les comportements de sortie stdout et fichier ;
- documenter l'inclusion optionnelle du mot de passe dans les NZB regeneres ;
- expliquer les avertissements lies aux articles `unknown` ;
- expliquer les limites d'une reprise apres crash ou fichiers sources manquants ;
- documenter la conservation, le masquage, l'export et la purge des mots de
  passe ;
- indiquer clairement que l'historique peut contenir des donnees sensibles ;
- donner des exemples simples pour les usages courants.

Exemples CLI a documenter :

```bash
ngPost --history
ngPost --history --status partial
ngPost --history-show <post-id>
ngPost --regenerate-nzb <post-id>
ngPost --regenerate-nzb <post-id> -o /path/post.nzb --include-password
ngPost --resume-list
ngPost --resume-post <post-id> --dry-run
ngPost --resume-post <post-id> --yes
ngPost --resume-abandon <post-id>
ngPost --resume-purge <post-id> --yes
```

## Jalons fonctionnels proposes

### Jalon 1 - Historique durable minimal

- Enregistrer les posts termines dans un historique structure.
- Garder le CSV existant en parallele.
- Distinguer `success`, `partial`, `failed` et `cancelled`.
- Historiser la presence, l'origine et, si autorisee, la valeur du mot de passe.
- Ajouter les champs necessaires aux stats globales.
- Mettre a jour les documentations minimales de configuration et de securite.

### Jalon 2 - Vue Historique IHM

- Ajouter la liste des posts.
- Ajouter recherche et filtres principaux.
- Ajouter le panneau detail.
- Ajouter affichage masque, copie et purge du mot de passe.
- Ajouter export CSV depuis l'IHM.
- Documenter la vue Historique et ses filtres.

### Jalon 3 - Stats

- Ajouter volume poste par jour.
- Ajouter vitesse moyenne.
- Ajouter nombre d'echecs.
- Ajouter filtres par periode et groupe.

### Jalon 4 - Historique article par article

- Historiser fichiers et articles.
- Stocker les Message-ID finaux.
- Marquer les articles incertains.
- Consolider les statuts de post et de fichier.

### Jalon 5 - Regeneration de NZB

- Regenerer un NZB depuis l'historique via CLI.
- Regenerer un NZB depuis l'IHM.
- Avertir sur les posts partiels ou incertains.
- Gerer explicitement l'inclusion ou l'omission du mot de passe.
- Exporter vers stdout ou fichier.
- Documenter les exemples CLI de regeneration de NZB.

### Jalon 6 - Reprise de post

- Ajouter le centre de reprise dans l'IHM.
- Ajouter les commandes CLI de listing, dry-run, reprise, abandon et purge.
- Reprendre les posts `resumable`.
- Proposer la reprise des posts `partial` quand leurs donnees le permettent.
- Signaler clairement les posts `partial` non reprenables.
- Reprendre apres crash.
- Reposter uniquement ce qui manque ou ce qui est incertain.
- Finaliser le NZB depuis l'historique consolide.
- Documenter le centre de reprise IHM et les commandes CLI de reprise.

## Questions ouvertes

- Faut-il historiser les mots de passe par defaut, ou rendre cela opt-in ?
- Faut-il chiffrer les mots de passe stockes, ou seulement documenter que la
  base d'historique est sensible ?
- Faut-il autoriser la recherche exacte par mot de passe ?
- Faut-il inclure le mot de passe dans les NZB regeneres par defaut ou demander
  une confirmation a chaque fois ?
- Quelle duree de retention proposer dans l'IHM ?
- Faut-il une commande de purge automatique ?
- Comment nommer officiellement la nouvelle option de stockage ?
- Faut-il permettre plusieurs bases d'historique ?
- Comment presenter simplement les articles `unknown` aux utilisateurs non techniques ?
- Faut-il permettre de fusionner plusieurs tentatives d'un meme post ?

## Questions non répondues

- Choix final du schema SQL.
- Choix exact des classes C++.
- Details de migration Qt SQL.
- Design precis des ecrans.
- Strategie de tests.
- Changements de packaging.

Ces points seront à traiter dans le plan d'implementation.
