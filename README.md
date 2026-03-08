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
│   └── Control_Hub_War_Thunder.ino   ← Code Arduino (LVGL + USB HID)
├── pc_bridge/
│   └── wt_telemetry.py               ← Script Python (API War Thunder → Serial)
└── README.md
```

---

## 🛒 Matériel requis

| Composant | Référence | Description |
|---|---|---|
| Carte | Arduino GIGA R1 WiFi (ABX00063) | STM32H747XI dual-core, USB-C |
| Écran | Arduino GIGA Display Shield (ASX00039) | 3.97", 800×480, tactile capacitif, IMU |
| Câble | USB-C vers USB-A | Connexion PC ↔ Arduino |

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

Puis éditez `pc_bridge/wt_telemetry.py` si nécessaire pour ajuster le port série (détection automatique disponible), et lancez :

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

## 🎯 Raccourcis configurés (War Thunder — Mode Chars)

| Bouton | Touche | Action in-game |
|---|---|---|
| EXTINCTEUR | `6` | Activer l'extincteur automatique |
| FUMIGENE | `G` | Lancer les grenades fumigènes |
| ARTILLERIE | `5` | Appel artillerie / marqueur |
| JUMELLES | `B` | Activer les jumelles |
| VUE TIREUR | `Shift` | Passer en vue lunette tireur (sniper) |
| MOTEUR | `I` | Couper / démarrer le moteur |
| RÉPARATION | `F` | Réparer le véhicule |

> 💡 Ces raccourcis sont ceux par défaut de War Thunder. Si vous les avez personnalisés, modifiez les callbacks `cb_*` dans le fichier `.ino`.

---

## 📡 API War Thunder — Données disponibles

War Thunder expose ses données télémétriques sur `http://127.0.0.1:8111` pendant une partie.

### Endpoints principaux

| Endpoint | Description |
|---|---|
| `/indicators` | Données instruments en temps réel (vitesse, RPM, rapport...) |
| `/state` | État général de la session (valide/en pause) |
| `/map_info.json` | Informations sur la carte en cours |
| `/hudmsg` | Messages HUD en jeu (hits, kills...) |

### Clés disponibles sur `/indicators` pour les chars (`army: tank`)

| Clé JSON | Type | Description |
|---|---|---|
| `speed` | float (m/s) | Vitesse du véhicule **(× 3.6 pour km/h)** |
| `rpm` | float | Régime moteur en tours/minute |
| `gear` | float | Rapport de vitesse engagé |
| `gear_neutral` | float | Rapport neutre (même valeur que gear si neutre) |
| `first_stage_ammo` | float | Munitions restantes dans le chargeur automatique |
| `crew_current` | float | Nombre de membres d'équipage en vie |
| `crew_total` | float | Nombre total de membres d'équipage |
| `crew_distance` | float | Distance de l'équipage (usage interne) |
| `stabilizer` | float | Stabilisateur actif (1.0) ou non (0.0) |
| `cruise_control` | float | Régulateur de vitesse actif (1.0) ou non (0.0) |
| `driver_state` | float | État du conducteur (0 = vivant) |
| `gunner_state` | float | État du tireur (0 = vivant) |
| `driving_direction_mode` | float | Mode direction (0 = normal) |
| `has_speed_warning` | float | Alerte de vitesse active (1.0) |
| `ircm` | float | Contre-mesures infrarouges (-1 = absent) |
| `lws` | float | Laser Warning System (-1 = absent) |
| `army` | string | Type de véhicule (`tank`, `aviation`, `ship`) |
| `type` | string | Modèle exact du véhicule (ex: `tankModels/us_m1a2_abrams`) |
| `valid` | bool | Données valides (true en partie active) |

> ⚠️ **Note** : Les clés disponibles varient selon le type de véhicule. Certaines clés comme `ircm` et `lws` renvoient `-1` si le système n'est pas présent sur le char. Le endpoint `/state` retourne `valid: false` en dehors d'une partie active.

---

## 🔧 Format du protocole Serial (Arduino ↔ Python)

Le script Python envoie des lignes texte sur le port série au format :

```
SPD:{int}|RPM:{int}|GEAR:{val}|AMMO:{int}|STAB:{0/1}|CREW:{n}/{total}|TANK:{nom}|STATUS:{0/1}\n
```

**Exemple en partie :**
```
SPD:42|RPM:2100|GEAR:4.0|AMMO:18|STAB:1|CREW:4/4|TANK:US_M1A2_ABRAMS|STATUS:1
```

L'Arduino parse cette chaîne dans `parse_and_update()` et met à jour l'interface LVGL en conséquence.

---

## 🖥️ Interface — Aperçu

### Écran 1 — Panneau de commandes
- Fond noir mat (Carbone)
- Barre HUD verte (type terminal militaire) affichant le modèle de char, vitesse et état équipage
- 6 boutons tactiles de couleurs militaires (Olive Drab, Rouge Danger, Gris Acier)
- Bouton RÉPARATION vertical sur la droite
- Bouton **TELEMETRY** (haut droite) → transition animée vers l'écran 2

### Écran 2 — Télémétrie
- Indicateur de connexion `[OK] PC BRIDGE: ONLINE` (vert) / `[!!] OFFLINE` (rouge)
- 3 blocs de données : Vitesse, RPM, Rapport
- Flux brut de données en bas
- Bouton retour vers l'écran 1

---

## 🏠 Boîtier 3D recommandé

Pour un usage bureau optimal, nous recommandons de modéliser (Fusion 360, FreeCAD) un boîtier incliné à **30–45°** en **PLA ou PETG** avec :
- Standoffs M3 internes pour fixer la carte
- Encoche USB-C sur la tranche
- Fentes d'aération style grille de blindage
- Texture "Fuzzy Skin" dans le slicer pour un rendu métallique

---

## 📄 Licence

MIT License — Libre d'utilisation, de modification et de distribution.
