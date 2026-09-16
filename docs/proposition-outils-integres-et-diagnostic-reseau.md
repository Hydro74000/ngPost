# Chemins des outils et diagnostic des connexions

Implémentation du 16 septembre 2026, à la suite de l’analyse de `3895102`.

## Réglages des outils

Les paramètres PAR2 et les paramètres de compression proposent désormais deux
choix distincts : **Outil**, puis **Chemin** (`Path` en anglais).

```text
Outil                  [ ParPar                         ▾ ]
Chemin                 [ Automatique (recommandé)       ▾ ]
                       Inclus avec ngPost · prêt
                       [ Afficher le chemin ]
```

- **Automatique (recommandé)** : recherche le moteur choisi dans le bundle courant,
  puis sur le système (`PATH` et emplacements Windows pris en charge). Le statut
  distingue un outil inclus, un outil trouvé sur cet ordinateur et un outil absent.
- **Personnalisé…** : affiche le champ de chemin et le bouton **Parcourir…**.
  Le chemin doit désigner un fichier exécutable pour enregistrer ce choix.
- **Afficher le chemin** permet de consulter le résultat de la détection sans le
  transformer en préférence persistante.
- Le moteur choisi est respecté : un ParPar absent n’est pas remplacé par
  par2cmdline avec les mêmes arguments. Le choix historique du moteur `auto`
  conserve sa détection ; enregistrer les réglages générés pour un moteur trouvé
  fixe ce moteur pour les lancements suivants.
- La compression propose **RAR** ou **7-Zip**. Un changement de moteur efface les
  arguments supplémentaires du compresseur précédent, qui peuvent être incompatibles.
- Un outil absent ne déclenche pas d’erreur au démarrage et n’empêche pas les posts
  qui n’en ont pas besoin. Son statut indique comment le rendre disponible.
- Avant toute compression, ngPost vérifie les outils demandés par le post, y compris
  le générateur PAR2. Un outil requis absent arrête ce post avec un message explicite.
  Un exécutable impossible à lancer, par exemple à cause d’un interpréteur absent,
  termine aussi le post au lieu de le laisser bloqué en préparation.

### Configuration et migration

```ini
PAR2_TOOL = parpar
PAR2_SOURCE = auto
RAR_TOOL = rar
RAR_SOURCE = auto
```

Les chemins résolus ne sont pas enregistrés en mode automatique. Pour imposer un
exécutable personnel, utiliser par exemple :

```ini
PAR2_TOOL = parpar
PAR2_SOURCE = custom
PAR2_PATH = /opt/outils/parpar
```

`RAR_TOOL` accepte `rar` ou `7zip`. `PAR2_TOOL` accepte `auto`, `parpar`,
`par2cmdline` ou `multipar`. Les deux clés `*_SOURCE` acceptent `auto` ou `custom`.
Les options CLI `--rar_path` et `--par2_path` restent des substitutions ponctuelles.

La lecture d’une ancienne configuration sans `*_SOURCE` reconnaît les exécutables
connus placés près du binaire et les chemins complets des montages AppImage ngPost
(`…/.mount_ngpost…/usr/bin/parpar`, par exemple). Elle retrouve le moteur et passe
ce chemin en automatique, même si l’ancien montage existe encore. Les chemins
personnalisés et les références ambiguës sont conservés. Une préférence `custom`
explicite ne fait jamais l’objet de cette conversion.

La correction s’applique immédiatement en mémoire. Elle est persistée lors de la
prochaine sauvegarde normale de configuration, par le mécanisme atomique existant.
La lecture seule de la configuration n’empêche donc pas de retrouver le bundle courant.

### Origine du défaut AppImage

`2498d9a` marquait le chemin comme modifié lors d’un simple changement de moteur
PAR2, puis sauvegardait le chemin détecté. `984295b` signalait ce chemin obsolète
et recherchait un outil de remplacement, mais conservait la référence périmée.
La compression sauvegardait aussi son chemin résolu. Le nouveau résolveur commun
sépare le moteur, le choix automatique/personnalisé et le chemin d’exécution.

## Logs et reconnexions

Les entrées du journal des posts portent un horodatage local **[HH:mm:ss.zzz]**,
y compris les erreurs, les traces debug et les sorties des outils externes.
Les lignes multiples sont horodatées individuellement. Les morceaux d’une même
ligne de progression partagent un horodatage ; les retours CR/LF coupés entre deux
lectures sont traités sans doublons. Les entrées déjà horodatées gardent leur préfixe.
Le fichier de log reçoit aussi les fragments debug. Les sorties JSON restent des données.

Les coupures d’une connexion établie pour lesquelles ngPost tente une reconnexion
ne positionnent plus immédiatement le post en erreur définitive :

- en mode normal, un résumé est regroupé par serveur et par fenêtre d’une seconde ;
- en debug, chaque tentative indique l’heure, le serveur/port, la connexion et la
  cause socket disponible ;
- les erreurs terminales, les échecs TLS et les articles non confirmés continuent
  d’être traités comme des erreurs.

Le nombre affiché au départ désigne maintenant les connexions **configurées**,
avant leur ouverture. Il ne prétend plus compter des connexions déjà disponibles.

Le défaut corrigé était reproductible : des coupures suivies d’une récupération
complète laissaient auparavant un NZB complet mais un code de sortie CLI 1.

## Vérification des changements VPN récents

L’analyse des commits du 11 au 15 septembre n’a pas identifié de modification du
cœur NNTP, du bind réseau ou du résolveur DNS VPN dans `9c2d355..3895102`.
Les changements VPN récents concernaient surtout WireGuard Windows et les contrôles
d’entrée du helper Linux. Les connexions NNTP s’ouvrent après la préparation des
archives : elles n’expirent donc pas pendant cette compression.

Le code 255 correspond à l’annulation confirmée par l’utilisateur. Le comportement
d’annulation n’a pas été modifié dans cette intervention.

Les tests locaux ne reproduisent pas le fournisseur Usenet ni le tunnel OpenVPN
« SS Albania ». Ils ne permettent pas d’attribuer la fermeture distante à un quota,
au serveur ou au trajet VPN ; le message socket seul n’en donne pas la cause.

## Validation

Couverture ajoutée : résolution après déplacement du bundle, reconnaissance stricte
des anciens chemins, respect du moteur et des chemins personnels, sauvegarde/relecture,
outil absent, exécutable impossible à démarrer avec conservation des sources,
logs multilignes et fragmentés, reconnexion récupérée en modes normal et debug.

Suites utilisées : `tst_Par2Settings`, `tst_MainWindow`, `tst_PostFlow`,
`tst_CliParser`, `tst_VpnProfile`, `tst_VpnSocketBinder` et `tst_VpnDnsResolver`.
Construction hors du dépôt dans `~/.cache/ngpost-audit-20260915/`, Qt 6.11.1 / GCC 16.
ParPar et le test GPU réel nécessitent des outils/périphériques absents de cet environnement.
Les sept traductions sont mises à jour et leurs catalogues compilés.

Résultat : **252 tests réussis, 0 échec, 2 ignorés** (ParPar et GPU réel).
Compilation de l’application réussie ; extraction des traductions complète.

Deux sondes supplémentaires ont envoyé 64 Mio sur 75 connexions TCP locales :

| Scénario | Articles confirmés / attendus | Sortie CLI |
|---|---:|---:|
| Sans coupure | 94 / 94 | 0 |
| 75 coupures avant confirmation, puis reconnexions | 94 / 94 | 0 |

Les deux NZB contiennent les 94 segments confirmés. Le second scénario produit
le résumé des 75 interruptions, sans marquer le post récupéré comme un échec.
