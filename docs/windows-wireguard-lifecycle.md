# WireGuard Windows : import et arrêt

L’import attend la fin du processus WireGuard, configure `start=demand`,
vérifie ce réglage et attend `SERVICE_STOPPED` avant de publier le succès.
L’ACL donne au SID de l’appelant les droits de lecture de configuration et
d’état, de démarrage et d’arrêt. Réimporter les profils préexistants pour
actualiser le type de démarrage et ces droits.

Le runtime utilise le SCM natif. Un service encore automatique est refusé au
démarrage. À la déconnexion, pendant un échec de démarrage, une perte du
watchdog ou un échec de bind, le backend conserve son identité et le verrou
jusqu’à confirmation `SERVICE_STOPPED` (ou absence certaine du service).
`STOP_PENDING`, refus d’accès et erreurs de requête ne constituent jamais
un arrêt confirmé. Les requêtes d’arrêt sont reprises sans bloquer l’interface.
L’interface reste en cours d’arrêt et les nouveaux démarrages sont bloqués.

À la fermeture, l’attente synchrone est bornée par le délai demandé ; si le
service reste actif, le watchdog indépendant est conservé. Un compte auquel
le droit d’arrêt a été retiré ne peut pas arrêter le service par magie : une
intervention administrateur peut alors être nécessaire. Aucun message de
déconnexion réussie n’est émis pour cet échec.

La suppression vérifie également l’arrêt avant suppression, et seul le code
SCM 1060 vaut « absent ». Les tests portables couvrent les transitions et le
maintien du backend ; les tests natifs Windows couvrent le parseur Win32,
PowerShell `-File` et le SCM sans installation privilégiée de service.
Le cycle réel import/UAC/service et la révocation d’ACL restent à valider sur
Windows. Aucun support VPN macOS n’est ajouté.
