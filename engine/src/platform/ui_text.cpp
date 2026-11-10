#include "wp/ui_text.h"

#include <atomic>
#include <cstring>

namespace wp::ui {

namespace {

constexpr Label kEnglishLabels[] = {
    {"video.scale", "Internal resolution"},
    {"video.fps", "Frames per second"},
    {"video.fullscreen", "Full screen"},
    {"video.copy_filter", "Anti-flicker filter"},
    {"video.custom_textures", "Custom textures"},
    {"video.dump_textures", "Dump textures"},
    {"input.gamepads", "Gamepads"},
    {"input.joycon_pairs", "Join Joy-Con pairs"},
    {"input.real_wiimotes", "Real Wii Remotes"},
    {"input.real_wiimote_mouse", "Aim Wii Remote 1 with the mouse"},
    {"input.wake_on_mouse", "Reconnect with the mouse"},
    {"input.hide_cursor", "Hide the mouse cursor"},
    {"system.interface_language", "Interface language"},
    {"system.language", "Console language"},
    {"system.pal60", "60 Hz (PAL60)"},
    {"system.options_menu", "Options menu (F1)"},
    {"system.discord", "Show the game in Discord"},
    {"audio.mute", "Mute"},
    {"audio.wiimote_speaker", "Wii Remote speaker"},
    {"saves.backups", "Save backups kept"},
    {"input.player1_keyboard_only", "Player 1: keyboard and mouse only"},
    {"video.vsync", "Vertical sync"},
    {"video.show_fps", "Frame rate in the title bar"},
    {"video.smooth", "Anti-aliasing"},
    {"video.anisotropy", "Anisotropic filtering"},
    {"system.widescreen", "Widescreen (16:9)"},
    {"input.rumble", "Vibration"},
    {"input.stick_deadzone", "Stick dead zone"},
    {"audio.volume", "Volume"},
    {"tab.display", "Display"},
    {"tab.keyboard", "Keyboard"},
    {"tab.gamepad", "Gamepad"},
    {"tab.audio", "Sound"},
    {"tab.system", "Game"},
    {"menu.auto", "Automatic"},
    {"menu.press_key", "Press a key..."},
    {"menu.press_button", "Press a button..."},
    {"menu.reset", "Reset"},
    {"keys.reset", "Default keys"},
    {"pad.reset", "Default buttons"},
    {"keys.a", "A button"},
    {"keys.b", "B button"},
    {"keys.one", "1 button"},
    {"keys.two", "2 button"},
    {"keys.plus", "+ button"},
    {"keys.minus", "- button"},
    {"keys.home", "HOME button"},
    {"keys.up", "Up"},
    {"keys.down", "Down"},
    {"keys.left", "Left"},
    {"keys.right", "Right"},
    {"keys.shake", "Shake"},
    {"keys.swing_up", "Swing up"},
    {"keys.swing_down", "Swing down"},
    {"keys.tilt_left", "Tilt left"},
    {"keys.tilt_right", "Tilt right"},
    {"keys.tilt_up", "Tilt up"},
    {"keys.tilt_down", "Tilt down"},
    {"keys.screenshot", "Screenshot"},
    {"keys.mouse_motion", "Motion with the mouse (hold)"},
    {"keys.pointer", "Pointer (hold)"},
    {"pad.use", "Use this controller"},
    {"pad.player", "Player"},
    {"pad.player_auto", "Automatic"},
    {"pad.view_title", "Controller view"},
    {"pad.view_none", "Connect a controller to see its buttons and sticks here."},
    {"pad.cross", "Cross"},
    {"pad.circle", "Circle"},
    {"pad.square", "Square"},
    {"pad.triangle", "Triangle"},
    {"pad.right_stick_up", "Right stick up"},
    {"pad.right_stick_down", "Right stick down"},
    {"pad.right_stick_left", "Right stick left"},
    {"pad.right_stick_right", "Right stick right"},
    {"pad.motion_help", "Motion without a gyroscope: the left trigger shakes. Hold the left shoulder button and push the right stick to tilt, or press up or down on the D-pad to swing. Each motion can also be given its own button below."},
};

constexpr Label kFrenchLabels[] = {
    {"video.scale", "Résolution interne"},
    {"video.fps", "Images par seconde"},
    {"video.fullscreen", "Plein écran"},
    {"video.copy_filter", "Filtre anti-scintillement"},
    {"video.custom_textures", "Textures personnalisées"},
    {"video.dump_textures", "Extraire les textures"},
    {"video.vsync", "Synchronisation verticale"},
    {"video.show_fps", "Images/s dans la barre de titre"},
    {"video.smooth", "Anticrénelage"},
    {"video.anisotropy", "Filtrage anisotrope"},
    {"system.widescreen", "Écran large (16:9)"},
    {"input.gamepads", "Manettes"},
    {"input.joycon_pairs", "Associer les paires de Joy-Con"},
    {"input.real_wiimotes", "Vraies Wiimotes"},
    {"input.real_wiimote_mouse", "Viser avec la souris (Wiimote 1)"},
    {"input.wake_on_mouse", "Reconnecter avec la souris"},
    {"input.hide_cursor", "Masquer le curseur"},
    {"input.rumble", "Vibrations"},
    {"input.stick_deadzone", "Zone morte du stick"},
    {"system.interface_language", "Langue du menu"},
    {"system.language", "Langue du jeu"},
    {"system.pal60", "60 Hz (PAL60)"},
    {"system.options_menu", "Menu des options (F1)"},
    {"system.discord", "Afficher le jeu sur Discord"},
    {"audio.mute", "Couper le son"},
    {"audio.volume", "Volume"},
    {"audio.wiimote_speaker", "Haut-parleur de la Wiimote"},
    {"saves.backups", "Copies de sauvegarde gardées"},
    {"input.player1_keyboard_only", "Joueur 1 : clavier et souris seulement"},
    {"tab.display", "Affichage"},
    {"tab.keyboard", "Clavier"},
    {"tab.gamepad", "Manette"},
    {"tab.audio", "Son"},
    {"tab.system", "Jeu"},
    {"menu.auto", "Automatique"},
    {"menu.press_key", "Appuie sur une touche..."},
    {"menu.press_button", "Appuie sur un bouton..."},
    {"menu.reset", "Réinitialiser"},
    {"keys.reset", "Touches par défaut"},
    {"pad.reset", "Boutons par défaut"},
    {"keys.a", "Bouton A"},
    {"keys.b", "Bouton B"},
    {"keys.one", "Bouton 1"},
    {"keys.two", "Bouton 2"},
    {"keys.plus", "Bouton +"},
    {"keys.minus", "Bouton -"},
    {"keys.home", "Bouton HOME"},
    {"keys.up", "Haut"},
    {"keys.down", "Bas"},
    {"keys.left", "Gauche"},
    {"keys.right", "Droite"},
    {"keys.shake", "Secouer"},
    {"keys.swing_up", "Agiter vers le haut"},
    {"keys.swing_down", "Agiter vers le bas"},
    {"keys.tilt_left", "Pencher à gauche"},
    {"keys.tilt_right", "Pencher à droite"},
    {"keys.tilt_up", "Pencher en avant"},
    {"keys.tilt_down", "Pencher en arrière"},
    {"keys.screenshot", "Capture d'écran"},
    {"keys.mouse_motion", "Mouvements à la souris (maintenir)"},
    {"keys.pointer", "Pointeur (maintenir)"},
    {"pad.use", "Utiliser cette manette"},
    {"pad.player", "Joueur"},
    {"pad.player_auto", "Automatique"},
    {"pad.view_title", "Vue de la manette"},
    {"pad.view_none", "Branche une manette pour voir ici ses boutons et ses sticks."},
    {"pad.cross", "Croix"},
    {"pad.circle", "Rond"},
    {"pad.square", "Carré"},
    {"pad.triangle", "Triangle"},
    {"pad.right_stick_up", "Stick droit haut"},
    {"pad.right_stick_down", "Stick droit bas"},
    {"pad.right_stick_left", "Stick droit gauche"},
    {"pad.right_stick_right", "Stick droit droite"},
    {"pad.motion_help", "Mouvements sans gyroscope : la gâchette gauche secoue. Maintiens la touche L1/LB et pousse le stick droit pour pencher, ou appuie en haut ou en bas sur la croix pour agiter. Chaque mouvement peut aussi avoir son propre bouton plus bas."},
};

constexpr Label kSpanishLabels[] = {
    {"video.scale", "Resolución interna"},
    {"video.fps", "Fotogramas por segundo"},
    {"video.fullscreen", "Pantalla completa"},
    {"video.copy_filter", "Filtro anti-parpadeo"},
    {"video.custom_textures", "Texturas personalizadas"},
    {"video.dump_textures", "Volcar texturas"},
    {"input.gamepads", "Mandos"},
    {"input.joycon_pairs", "Unir parejas de Joy-Con"},
    {"input.real_wiimotes", "Wii Remotes reales"},
    {"input.real_wiimote_mouse", "Apuntar el Wii Remote 1 con el ratón"},
    {"input.wake_on_mouse", "Reconectar con el ratón"},
    {"input.hide_cursor", "Ocultar el cursor del ratón"},
    {"system.interface_language", "Idioma de la interfaz"},
    {"system.language", "Idioma de la consola"},
    {"system.pal60", "60 Hz (PAL60)"},
    {"system.options_menu", "Menú de opciones (F1)"},
    {"audio.mute", "Silenciar"},
    {"audio.wiimote_speaker", "Altavoz del Wii Remote"},
    {"saves.backups", "Copias de la partida"},
    {"input.player1_keyboard_only", "Jugador 1: solo teclado y ratón"},
    {"pad.use", "Usar este mando"},
    {"pad.player", "Jugador"},
    {"pad.player_auto", "Automático"},
    {"pad.view_title", "Vista del mando"},
    {"pad.view_none", "Conecta un mando para ver aquí sus botones y sticks."},
    {"pad.cross", "Cruz"},
    {"pad.circle", "Círculo"},
    {"pad.square", "Cuadrado"},
    {"pad.triangle", "Triángulo"},
    {"pad.right_stick_up", "Stick derecho arriba"},
    {"pad.right_stick_down", "Stick derecho abajo"},
    {"pad.right_stick_left", "Stick derecho izquierda"},
    {"pad.right_stick_right", "Stick derecho derecha"},
    {"pad.motion_help", "Movimiento sin giroscopio: el gatillo izquierdo sacude. Mantén el botón superior izquierdo y mueve el stick derecho para inclinar, o pulsa arriba o abajo en la cruceta para golpear. Cada movimiento también puede tener su propio botón más abajo."},
};

constexpr Label kGalicianLabels[] = {
    {"video.scale", "Resolución interna"},
    {"video.fps", "Fotogramas por segundo"},
    {"video.fullscreen", "Pantalla completa"},
    {"video.copy_filter", "Filtro antiparpadeo"},
    {"video.custom_textures", "Texturas personalizadas"},
    {"video.dump_textures", "Envorcar texturas"},
    {"input.gamepads", "Mandos"},
    {"input.joycon_pairs", "Unir parellas de Joy-Con"},
    {"input.real_wiimotes", "Wii Remotes reais"},
    {"input.real_wiimote_mouse", "Apuntar o Wii Remote 1 co rato"},
    {"input.wake_on_mouse", "Reconectar co rato"},
    {"input.hide_cursor", "Agochar o cursor do rato"},
    {"system.interface_language", "Idioma da interface"},
    {"system.language", "Idioma da consola"},
    {"system.pal60", "60 Hz (PAL60)"},
    {"system.options_menu", "Menú de opcións (F1)"},
    {"audio.mute", "Silenciar"},
    {"audio.wiimote_speaker", "Altofalante do Wii Remote"},
    {"saves.backups", "Copias da partida"},
    {"input.player1_keyboard_only", "Xogador 1: só teclado e rato"},
    {"pad.use", "Usar este mando"},
    {"pad.player", "Xogador"},
    {"pad.player_auto", "Automático"},
    {"pad.view_title", "Vista do mando"},
    {"pad.view_none", "Conecta un mando para ver aquí os seus botóns e sticks."},
    {"pad.cross", "Cruz"},
    {"pad.circle", "Círculo"},
    {"pad.square", "Cadrado"},
    {"pad.triangle", "Triángulo"},
    {"pad.right_stick_up", "Stick dereito arriba"},
    {"pad.right_stick_down", "Stick dereito abaixo"},
    {"pad.right_stick_left", "Stick dereito esquerda"},
    {"pad.right_stick_right", "Stick dereito dereita"},
    {"pad.motion_help", "Movemento sen xiroscopio: o gatillo esquerdo sacode. Mantén o botón superior esquerdo e move o stick dereito para inclinar, ou preme arriba ou abaixo na cruceta para golpear. Cada movemento tamén pode ter o seu propio botón máis abaixo."},
};

constexpr Text kEnglish{"FPS",
                        "DSP",
                        "NATIVE",
                        "INTERP",
                        "OFF",
                        "Options",
                        "Up/Down: choose   Left/Right: change   Q/E or LB/RB: tab   Enter: bind   Esc or F1: close",
                        "applies on restart",
                        "On",
                        "Off",
                        "Native",
                        "System",
                        kEnglishLabels,
                        sizeof(kEnglishLabels) / sizeof(kEnglishLabels[0])};

constexpr Text kFrench{"FPS",
                       "DSP",
                       "NATIF",
                       "INTERP",
                       "ARRÊT",
                       "Options",
                       "Haut/Bas : choisir   Gauche/Droite : changer   Q/E ou LB/RB : onglet   Entrée : assigner   Échap ou F1 : fermer",
                       "au prochain lancement",
                       "Oui",
                       "Non",
                       "Native",
                       "Système",
                       kFrenchLabels,
                       sizeof(kFrenchLabels) / sizeof(kFrenchLabels[0])};

constexpr Text kSpanish{"FPS",
                        "DSP",
                        "NATIVO",
                        "INTERP",
                        "APAGADO",
                        "Opciones",
                        "Arriba/Abajo: elegir   Izquierda/Derecha: cambiar   Q/E o LB/RB: pestaña   Esc o F1: cerrar",
                        "se aplica al reiniciar",
                        "Sí",
                        "No",
                        "Nativa",
                        "Sistema",
                        kSpanishLabels,
                        sizeof(kSpanishLabels) / sizeof(kSpanishLabels[0])};

constexpr Text kGalician{"FPS",
                         "DSP",
                         "NATIVO",
                         "INTERP",
                         "APAGADO",
                         "Opcións",
                         "Arriba/Abaixo: escoller   Esquerda/Dereita: cambiar   Q/E ou LB/RB: lapela   Esc ou F1: pechar",
                         "aplícase ao reiniciar",
                         "Si",
                         "Non",
                         "Nativa",
                         "Sistema",
                         kGalicianLabels,
                         sizeof(kGalicianLabels) / sizeof(kGalicianLabels[0])};

std::atomic<const Text*> g_text{&kEnglish};

const char* find_label(const Text& current, const char* key) {
    for (size_t i = 0; i < current.label_count; i++) {
        if (std::strcmp(current.labels[i].key, key) == 0) {
            return current.labels[i].text;
        }
    }
    return nullptr;
}

// A label missing from a translation falls back to English.
const char* label_in(const Text& current, const char* key) {
    const char* found = find_label(current, key);
    found = found ? found : find_label(kEnglish, key);
    return found ? found : key;
}

const Text& text_for(Language language) {
    switch (language) {
    case Language::Spanish:
        return kSpanish;
    case Language::Galician:
        return kGalician;
    case Language::French:
        return kFrench;
    default:
        return kEnglish;
    }
}

}

void set_language(Language language) {
    g_text = &text_for(language);
}

Language language_of(const std::string& code) {
    return code == "es" ? Language::Spanish : code == "gl" ? Language::Galician : code == "fr" ? Language::French : Language::English;
}

void set_language(const std::string& code) {
    set_language(language_of(code));
}

const Text& text() {
    return *g_text.load();
}

const char* label(const char* key) {
    return label_in(text(), key);
}

const char* label(const char* key, Language language) {
    return label_in(text_for(language), key);
}

}
