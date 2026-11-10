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
    {"system.options_menu", "Options menu (F10)"},
    {"audio.mute", "Mute"},
    {"audio.wiimote_speaker", "Wii Remote speaker"},
    {"saves.backups", "Save backups kept"},
    {"input.player1_keyboard_only", "Player 1: keyboard and mouse only"},
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
    {"system.options_menu", "Menú de opciones (F10)"},
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
    {"system.options_menu", "Menú de opcións (F10)"},
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
                        "Up/Down: choose     Left/Right/Enter or click: change     Esc or right click: close",
                        "applies on restart",
                        "On",
                        "Off",
                        "Native",
                        "System",
                        kEnglishLabels,
                        sizeof(kEnglishLabels) / sizeof(kEnglishLabels[0])};

constexpr Text kSpanish{"FPS",
                        "DSP",
                        "NATIVO",
                        "INTERP",
                        "APAGADO",
                        "Opciones",
                        "Arriba/Abajo: elegir     Izquierda/Derecha/Intro o clic: cambiar     Esc o clic derecho: cerrar",
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
                         "Arriba/Abaixo: escoller     Esquerda/Dereita/Intro ou clic: cambiar     Esc ou clic dereito: pechar",
                         "aplícase ao reiniciar",
                         "Si",
                         "Non",
                         "Nativa",
                         "Sistema",
                         kGalicianLabels,
                         sizeof(kGalicianLabels) / sizeof(kGalicianLabels[0])};

std::atomic<const Text*> g_text{&kEnglish};

const char* label_in(const Text& current, const char* key) {
    for (size_t i = 0; i < current.label_count; i++) {
        if (std::strcmp(current.labels[i].key, key) == 0) {
            return current.labels[i].text;
        }
    }
    return key;
}

const Text& text_for(Language language) {
    switch (language) {
    case Language::Spanish:
        return kSpanish;
    case Language::Galician:
        return kGalician;
    default:
        return kEnglish;
    }
}

}

void set_language(Language language) {
    g_text = &text_for(language);
}

Language language_of(const std::string& code) {
    return code == "es" ? Language::Spanish : code == "gl" ? Language::Galician : Language::English;
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
