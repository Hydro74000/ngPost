# Fiches de post

Après chaque post, ngPost peut écrire un petit fichier texte décrivant ce qui
vient d'être posté. Certains index Usenet réclament ce genre de fiche pour
référencer un post.

**ngPost ne connaît aucun format d'index.** Vous lui donnez un modèle, il
remplit les trous. C'est pour ça que ces fichiers sont ici et pas dans le code :
si un index change son format, seul son modèle change.

## S'en servir

1. Copiez le modèle voulu (ou son contenu) **là où vous pouvez le lire**. Il
   n'a pas besoin d'être dans ce dossier, ni dans aucun dossier particulier.
2. Ajoutez deux lignes à votre `ngPost.conf` :

```ini
POST_INFO_TEMPLATE = ma_fiche.txt
POST_INFO_OUTPUT = __nzbDir__/__nzbName__.info.txt
```

Un simple nom de fichier est compris depuis le dossier de votre `ngPost.conf`.
**Si le modèle est ailleurs, écrivez son chemin complet**, ngPost n'ira pas le
chercher :

```ini
POST_INFO_TEMPLATE = /home/moi/modeles/ma_fiche.txt
POST_INFO_TEMPLATE = C:\Users\moi\modeles\ma_fiche.txt
```

En ligne de commande, `--post_info_template` comprend au contraire un chemin
relatif depuis le dossier où vous vous trouvez.

3. Postez quelque chose, et regardez à côté de votre `.nzb`.

## Les modèles fournis

| Fichier | Ce que c'est |
|---|---|
| `post_info_default.txt` | générique, une ligne par variable disponible, sans mot de passe |
| `post_info_baselien.txt` | le format exact attendu par l'index Baselien |
| `post_info_json.txt` | une fiche JSON, pour un index doté d'une API HTTP |

Un modèle est du texte libre : il peut donc aussi être un document JSON ou XML.
Déclarez le format sur une ligne à lui — `#!json` ou `#!xml` — et ngPost échappe
chaque valeur qu'il insère, de sorte qu'un titre contenant un guillemet ou une
esperluette ne casse plus le fichier. Seules les valeurs sont échappées : les
accolades, les balises et les noms de champs sont à vous. Sans déclaration,
rien n'est échappé, ce qui permet à une fiche texte d'utiliser le séparateur
que vous voulez.

## Documentation complète

Le guide débutant complet, avec toutes les variables et un exemple de bout en
bout, est sur le wiki : **Fiches de post**.
