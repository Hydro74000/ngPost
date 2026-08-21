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

## Documentation complète

Le guide débutant complet, avec toutes les variables et un exemple de bout en
bout, est sur le wiki : **Fiches de post**.
