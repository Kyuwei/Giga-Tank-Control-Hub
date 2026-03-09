# 🎮 Giga Tank Control Hub

> Panneau de commandes tactile militaire pour simulateurs de chars — **Arduino GIGA R1 WiFi** + **GIGA Display Shield**

![Platform](https://img.shields.io/badge/Platform-Arduino%20GIGA%20R1%20WiFi-blue)
![Shield](https://img.shields.io/badge/Shield-GIGA%20Display%20800x480-green)
![Game](https://img.shields.io/badge/Game-War%20Thunder-red)
![License](https://img.shields.io/badge/License-MIT-yellow)

---

## 📋 Description

Ce projet transforme un **Arduino GIGA R1 WiFi** équipé de son **GIGA Display Shield** (écran tactile 800×480 px) en un véritable panneau de commande de char, inspiré des systèmes de contrôle embarqués des chars modernes (M1A2 Abrams, Leopard 2, etc.).

L'Arduino est reconnu par Windows comme un **clavier USB HID natif** (plug & play, sans driver). L'interface LVGL propose **deux écrans** :
- **Écran 1 — Commandes** : 6 gros boutons tactiles + 1 bouton Réparation pour déclencher les raccourcis clavier du jeu.
- **Écran 2 — Télémétrie** : Affichage en temps réel des données du véhicule (vitesse, RPM, rapport, équipage) lues depuis l'API locale de War Thunder via un script Python.

---

## 🏗️ Architecture du projet

```
Giga-Tank-Control-Hub/
├── Control_Hub_War_Thunder/
│   └── Control_Hub_War_Thunder.ino   <- Code Arduino (LVGL + USB HID)
├── pc_bridge/
│   └── wt_telemetry.py               <- Script Python (API War Thunder -> Serial)
└── README.md
```

---

## 🛒 Matériel requis

| Composant | Référence | Description |
|---|---|---|
| Carte | Arduino GIGA R1 WiFi (ABX00063) | STM32H747XI dual-core, USB-C |
| Écran | Arduino GIGA Display Shield (ASX00039) | 3.97", 800x480, tactile capacitif, IMU |
| Câble | USB-C vers USB-A | Connexion PC <-> Arduino |

---

## ⚙️ Installation

### 1. Arduino IDE

1. Installez **Arduino IDE 2.x**.
2. Dans le gestionnaire de cartes, installez : `Arduino Mbed OS Giga Boards`.
3. Dans le gestionnaire de bibliothèques, installez :
   - `Arduino_H7_Video`
   - `Arduino_GigaDisplayTouch`
   - `lvgl` (version 8.x recommandée)
   - `PluggableUSBHID` (incluse dans le pack Mbed)
4. Ouvrez `Control_Hub_War_Thunder/Control_Hub_War_Thunder.ino`.
5. Sélectionnez la carte **Arduino GIGA R1 WiFi** et le bon port COM.
6. Flashez.

### 2. Script Python (pont télémétrique)

```bash
pip install requests pyserial
```

Puis lancez :

```bash
python pc_bridge/wt_telemetry.py
```

---

## 🕹️ Utilisation

1. Branchez l'Arduino GIGA en USB-C sur votre PC.
2. Lancez War Thunder et entrez dans une partie ou un essai de véhicule avec un **char**.
3. Lancez le script Python.
4. L'écran principal affiche le panneau de commandes. Appuyez sur **TELEMETRY** (haut droite) pour voir les données en direct.

---

## 🎯 Raccourcis configurés (War Thunder - Mode Chars)

| Bouton | Touche | Action in-game |
|---|---|---|
| EXTINCTEUR | `6` | Activer l'extincteur automatique |
| FUMIGENE | `G` | Lancer les grenades fumigènes |
| ARTILLERIE | `5` | Appel artillerie / marqueur |
| JUMELLES | `B` | Activer les jumelles |
| VUE TIREUR | `Shift` | Passer en vue lunette tireur (sniper) |
| MOTEUR | `I` | Couper / démarrer le moteur |
| REPARATION | `F` | Réparer le véhicule |

> Ces raccourcis sont ceux par défaut de War Thunder. Si vous les avez personnalisés, modifiez les callbacks `cb_*` dans le fichier `.ino`.

---

## 📡 API War Thunder - Documentation complète (localhost:8111)

War Thunder expose ses données télémétriques sur `http://127.0.0.1:8111` automatiquement dès qu'une partie est lancée. Ce serveur local s'arrête à la fin de la session.

> **Avertissement** : Gaijin Entertainment ne garantit pas la stabilité de cette API entre les mises à jour. Les noms de clés peuvent changer selon la version du client ou le type de véhicule.

### Vue d'ensemble des endpoints

| Endpoint | Format | Description |
|---|---|---|
| `/indicators` | JSON | Données instruments en temps réel |
| `/state` | JSON | Etat général de la session |
| `/map_info.json` | JSON | Métadonnées de la carte en cours |
| `/map_obj.json` | JSON Array | Objets sur la carte (véhicules, objectifs) |
| `/map.img` | JPEG | Image bitmap de la carte (1024x1024 px) |
| `/mission.json` | JSON | Objectifs et statut de la mission |
| `/hudmsg` | JSON | Messages HUD (dégâts, kills, alertes) |
| `/gamechat` | JSON | Messages du chat en jeu |

---

### `/indicators` - Instruments (Chars)

Clés disponibles pour les véhicules terrestres (`army: tank`) :

| Clé JSON | Type | Description |
|---|---|---|
| `speed` | float (m/s) | Vitesse du véhicule (**x 3.6 pour km/h**) |
| `rpm` | float | Régime moteur en tours/minute |
| `gear` | float | Rapport de vitesse engagé |
| `gear_neutral` | float | Valeur du rapport neutre |
| `first_stage_ammo` | float | Munitions dans le chargeur automatique |
| `crew_current` | float | Membres d'équipage en vie |
| `crew_total` | float | Membres d'équipage total |
| `stabilizer` | float | Stabilisateur : `1.0` = actif, `0.0` = inactif |
| `cruise_control` | float | Régulateur de vitesse : `1.0` = actif |
| `driver_state` | float | Etat conducteur (`0` = vivant) |
| `gunner_state` | float | Etat tireur (`0` = vivant) |
| `has_speed_warning` | float | Alerte survitesse : `1.0` = active |
| `ircm` | float | Contre-mesures IR (`-1` = absent sur ce véhicule) |
| `lws` | float | Laser Warning System (`-1` = absent) |
| `driving_direction_mode` | float | Mode direction (`0` = normal) |
| `army` | string | Type : `tank`, `aviation`, `ship` |
| `type` | string | Modèle exact (ex: `tankModels/us_m1a2_abrams`) |
| `valid` | bool | `true` si les données sont valides |

---

### `/state` - Etat de la session

| Clé JSON | Type | Description |
|---|---|---|
| `valid` | bool | `true` si une partie est en cours |
| `isPaused` | bool | `true` si la partie est en pause |
| `isReplay` | bool | `true` si on regarde un replay |
| `isNetworkGame` | bool | `true` pour les parties en ligne |

> Utile pour détecter proprement le début et la fin d'une partie avant de lire `/indicators`.

---

### `/map_info.json` - Metadonnées de la carte

Cet endpoint retourne un objet JSON décrivant les **propriétés géographiques et dimensionnelles** de la carte en cours. Il est indispensable pour convertir les coordonnées relatives (entre 0.0 et 1.0) des objets retournés par `/map_obj.json` en positions réelles.

**Exemple de réponse JSON :**
```json
{
  "map_generation": 42,
  "map_min": [0.0, 0.0],
  "map_max": [1.0, 1.0]
}
```

| Clé JSON | Type | Description |
|---|---|---|
| `map_generation` | int | Numero de generation de la carte. S'incremente a chaque rechargement. Utile pour detecter un changement de map. |
| `map_min` | array [float, float] | Coordonnees normalisees du coin **superieur gauche** `[x_min, y_min]` (generalement `[0.0, 0.0]`) |
| `map_max` | array [float, float] | Coordonnees normalisees du coin **inferieur droit** `[x_max, y_max]` (generalement `[1.0, 1.0]`) |

**Convertir les coordonnees en pixels (image 1024x1024) :**
```python
pixel_x = obj['x'] * 1024
pixel_y = obj['y'] * 1024
```

**Detecter un changement de carte :**
```python
last_generation = 0
info = requests.get('http://127.0.0.1:8111/map_info.json').json()
if info['map_generation'] != last_generation:
    last_generation = info['map_generation']
    # Re-telecharger map.img et mettre a jour l'affichage
```

---

### `/map_obj.json` - Objets sur la carte

Retourne un **tableau JSON** contenant tous les objets affichés sur la minimap : vehicules allies, ennemis detectes, objectifs, points de capture, aerodromes, etc.

**Exemple d'un objet :**
```json
[
  {
    "type": "airfield",
    "icon": "airfield",
    "color": "#1a8a1a",
    "color2": "#1a8a1a",
    "blink": 0,
    "x": 0.213,
    "y": 0.445,
    "ex": 0.0,
    "ey": 0.0
  }
]
```

| Clé JSON | Type | Description |
|---|---|---|
| `type` | string | Type d'objet : `airfield`, `tank`, `aircraft`, `capture_zone`, `respawn_base_tank`, `bomb_point`, etc. |
| `icon` | string | Icone associee a cet objet sur la minimap |
| `color` | string (hex) | Couleur de l'objet. **Rouge** (`#f40c00`, `#ff0000`) = ennemi. **Vert** = allie. |
| `color2` | string (hex) | Couleur secondaire (objets bicolores) |
| `blink` | int | `1` = l'objet clignote (menace active ou objectif urgent), `0` = statique |
| `x` | float (0.0-1.0) | Position normalisee horizontale sur la carte |
| `y` | float (0.0-1.0) | Position normalisee verticale sur la carte |
| `ex` | float | Coordonnee X de l'extremite (objets allonges comme les pistes) |
| `ey` | float | Coordonnee Y de l'extremite |

**Detecter les ennemis en Python :**
```python
ENEMY_COLORS = ['#f40c00', '#ff0d00', '#ff0000']

def is_enemy(obj):
    return obj['color'].lower() in ENEMY_COLORS or obj['blink'] == 1

objects = requests.get('http://127.0.0.1:8111/map_obj.json').json()
enemies = [o for o in objects if is_enemy(o)]
print(f"{len(enemies)} ennemi(s) detecte(s) sur la carte")
```

---

### `/map.img` - Image de la carte

Retourne le **fichier JPEG de la minimap** en cours (resolution **1024x1024 pixels**). A combiner avec `/map_obj.json` pour afficher les positions des vehicules par-dessus.

```python
import requests
with open('map.jpg', 'wb') as f:
    f.write(requests.get('http://127.0.0.1:8111/map.img').content)
```

> L'image change a chaque nouvelle carte. Utilisez `map_generation` de `/map_info.json` pour detecter le changement et re-telecharger l'image automatiquement.

---

### `/mission.json` - Objectifs de la mission

| Clé JSON | Type | Description |
|---|---|---|
| `mission_success` | string | Statut global : `"success"`, `"fail"`, ou `"progress"` |
| `success_conditions` | array | Liste des conditions de victoire et leur etat |
| `fail_conditions` | array | Liste des conditions d'echec et leur etat |

> **Attention** : Ce endpoint est connu pour retourner des statuts errones (ex: `"success"` premature). A utiliser avec prudence pour la detection de fin de partie.

---

### `/hudmsg` - Messages HUD

| Clé JSON | Type | Description |
|---|---|---|
| `events` | array | Liste des evenements recents (kills, alertes...) |
| `events[].msg` | string | Texte du message (ex: `"You have destroyed an enemy"`) |
| `events[].sender` | string | Expediteur du message |
| `events[].enemy` | bool | `true` si concerne un ennemi |
| `damage` | array | Dommages infliges/recus |
| `damage[].msg` | string | Description du dommage (module touche, etc.) |
| `damage[].playerName` | string | Nom du joueur concerne |

---

### `/gamechat` - Chat en jeu

| Clé JSON | Type | Description |
|---|---|---|
| `messages` | array | Liste des messages |
| `messages[].msg` | string | Contenu du message |
| `messages[].sender` | string | Pseudonyme de l'expediteur |
| `messages[].enemy` | bool | `true` si le message vient du camp ennemi |
| `messages[].mode` | string | Mode du chat : `"Squad"`, `"Allies"`, `"All"` |

---

## 🔧 Format du protocole Serial (Arduino <-> Python)

Le script Python envoie des lignes texte sur le port serie au format :

```
SPD:{int}|RPM:{int}|GEAR:{val}|AMMO:{int}|STAB:{0/1}|CREW:{n}/{total}|TANK:{nom}|STATUS:{0/1}
```

**Exemple en partie :**
```
SPD:42|RPM:2100|GEAR:4.0|AMMO:18|STAB:1|CREW:4/4|TANK:US_M1A2_ABRAMS|STATUS:1
```

L'Arduino parse cette chaine dans `parse_and_update()` et met a jour l'interface LVGL en consequence.

---

## 🖥️ Interface - Apercu

### Ecran 1 - Panneau de commandes
- Fond noir mat (Carbone)
- Barre HUD verte (type terminal militaire) affichant le modele de char, vitesse et etat equipage
- 6 boutons tactiles de couleurs militaires (Olive Drab, Rouge Danger, Gris Acier)
- Bouton REPARATION vertical sur la droite
- Bouton **TELEMETRY** (haut droite) -> transition animee vers l'ecran 2

### Ecran 2 - Telemetrie
- Indicateur de connexion `[OK] PC BRIDGE: ONLINE` (vert) / `[!!] OFFLINE` (rouge)
- 3 blocs de donnees : Vitesse, RPM, Rapport
- Flux brut de donnees en bas
- Bouton retour vers l'ecran 1

---

## 🏠 Boitier 3D recommande

Pour un usage bureau optimal, modelisez (Fusion 360, FreeCAD) un boitier incline a **30-45 degres** en **PLA ou PETG** avec :
- Standoffs M3 internes pour fixer la carte
- Encoche USB-C sur la tranche
- Fentes d'aeration style grille de blindage
- Texture "Fuzzy Skin" dans le slicer pour un rendu metallique

---

## 📄 Licence

MIT License - Libre d'utilisation, de modification et de distribution.
