# Les jeux trop gros : où est la limite (notes de travail)

Mesures faites sur une cartouche de 32 Mo (pleine : 9,6 Mo de son, 23 Mo de reste), pour savoir
jusqu'où la calculatrice peut aller. Rien ici n'est encore dans l'outil.

## La place

| | Ko |
|---|---|
| les deux zones d'applis (aujourd'hui) | 4 990 |
| + le système de secours (`installer --secours`) | 6 400 |
| toute la puce, avec un système maison minimal (noyau de 64 Ko gardé) | ~8 000 |

## La taille du pack, selon la façon de compresser

Le programme (reconnu par un test strict : des appels qui tombent sur un début de fonction) fait
788 Ko et reste tel quel ; l'outil d'aujourd'hui en garde 2 400 Ko, faute de test assez strict.

| groupes | deflate | LZMA |
|---|---|---|
| 1 Ko (aujourd'hui, test lâche) | 11 400 | |
| 1 Ko, test strict | 10 700 | |
| 4 Ko | 9 350 | 8 920 |
| 16 Ko | 8 670 | 7 820 |
| 32 Ko | | 7 490 |
| 64 Ko | | 7 230 |
| d'un seul bloc (inutilisable) | | 5 940 |

Un dictionnaire partagé par région gagne 3 à 4 % sur les petits groupes ; les doublons exacts, 0,2 Mo.

## Combien de décompressions en jouant

Du démarrage à la première maison (14 000 images). Garder des groupes entiers en cache (128 Ko) ne
marche qu'à 4 Ko : à 16 Ko il n'y a que huit places et le jeu lit partout à la fois (plusieurs
décompressions par image, en permanence). Garder des *pages* de 256 octets (96 Ko) et le dernier
groupe décompressé change tout : 100 à 150 décompressions pour 2 000 images quelle que soit la
taille du groupe, 97 % des images sans aucune, 25 à 35 dans la pire image (un changement de scène).

## Le temps d'une décompression

Sur le processeur simulé, quinze groupes de 32 Ko pris dans le jeu : 2,26 millions d'instructions en
moyenne pour LZMA (`core/lzma.c`, vérifié contre xz), 0,76 pour deflate. Avec la règle calée sur la
calculatrice (6,5 ns par instruction), 15 ms par groupe LZMA entier, la moitié en moyenne si on
s'arrête à la page voulue : un changement de scène à trente décompressions gèlerait un quart à une
demi-seconde, et le reste du temps rien.

## Ce qu'il reste à savoir

- la mémoire : le jeu demande 120 Ko de vraie mémoire dans l'introduction ; avec un groupe de 32 Ko,
  48 à 96 Ko de pages et l'état du décompresseur, il faut plus que les 94 Ko d'une appli ordinaire ;
- le jeu au-delà de l'introduction (combats) ;
- tout ce qui touche au système maison : rien n'est essayé.
