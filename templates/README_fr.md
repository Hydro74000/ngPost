# Fiches de post

Après chaque post, ngPost peut écrire un petit fichier texte décrivant ce qui
vient d'être posté. Certains index Usenet réclament ce genre de fiche pour
référencer un post.

**ngPost ne connaît aucun format d'index.** Vous lui donnez un modèle, il
remplit les trous. C'est pour ça que ces fichiers sont ici et pas dans le code :
si un index change son format, seul son modèle change.

## S'en servir

1. Copiez le modèle voulu (ou son contenu) là où vous pouvez écrire, par
   exemple à côté de votre `ngPost.conf`.
2. Ajoutez deux lignes à votre `ngPost.conf` :

```ini
POST_INFO_TEMPLATE = ma_fiche.txt
POST_INFO_OUTPUT = __nzbDir__/__nzbName__.info.txt
```

Un chemin relatif est compris depuis le dossier de votre `ngPost.conf`.

3. Postez quelque chose, et regardez à côté de votre `.nzb`.

## Les modèles fournis

| Fichier | Ce que c'est |
|---|---|
| `post_info_default.txt` | générique, une ligne par variable disponible, sans mot de passe |
| `post_info_baselien.txt` | le format exact attendu par l'index Baselien |

## Documentation complète

Le guide débutant complet, avec toutes les variables et un exemple de bout en
bout, est sur le wiki : **Fiches de post**.
