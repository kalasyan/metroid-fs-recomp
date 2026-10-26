// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — in-game options menu, drawn with Dear ImGui over the paused
// game. Works with a gamepad (d-pad + south button, Start closes), keyboard
// (arrows + Enter, Esc closes), mouse and touch.
#include "port_menu.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <stdio.h>

namespace {

enum Page {
    PAGE_STATES,
    PAGE_CONTROLS,
    PAGE_VIDEO,
    PAGE_SOUND_SPEED,
    PAGE_TOUCH,
    PAGE_EXTRAS,
    PAGE_COUNT
};

const char* const kPageNames[PAGE_COUNT] = {
    "Guardados rápidos",
    "Controles",
    "Vídeo",
    "Sonido y velocidad",
    "Mando táctil",
    "Mapa y vibración",
};

const char* const kInputLabels[PORT_INPUT_COUNT] = {
    "A", "B", "Select", "Start", "Derecha", "Izquierda", "Arriba", "Abajo", "R", "L", "Avance rápido",
};

enum CaptureDevice {
    CAPTURE_NONE,
    CAPTURE_KEYBOARD,
    CAPTURE_GAMEPAD,
};

PortMenuHost sHost;
bool sReady;
bool sOpen;
int sPage = PAGE_STATES;
float sScale;
ImGuiStyle sBaseStyle;

// Rebinding
CaptureDevice sCapture = CAPTURE_NONE;
int sCaptureInput;
Uint64 sCaptureStartNs;
bool sNavHeld; // gamepad navigation off until every button is released

// Touch layout editor
bool sEditing;
int sEditSelected = -1;
bool sDragging;
float sDragDx, sDragDy;
ImVec2 sEditorMin, sEditorMax; // the editor's panel, in render pixels

char sStatus[128];

PortConfig& Config()
{
    return *sHost.config;
}

// Grey explanatory text that wraps to the page width.
void Hint(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void SetStatus(const char* text)
{
    snprintf(sStatus, sizeof(sStatus), "%s", text);
}

// ---- helpers --------------------------------------------------------------------

void UpdateScale()
{
    int w = 0, h = 0;
    if (!SDL_GetCurrentRenderOutputSize(sHost.renderer, &w, &h))
        SDL_GetWindowSizeInPixels(sHost.window, &w, &h);
    float scale = h / 380.0f;
    if (scale < 1.0f)
        scale = 1.0f;
    if (scale > 4.0f)
        scale = 4.0f;
    scale = SDL_floorf(scale * 4.0f) / 4.0f;
    if (scale == sScale)
        return;
    sScale = scale;
    ImGuiStyle& style = ImGui::GetStyle();
    style = sBaseStyle;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

bool AnyGamepadInputHeld()
{
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    bool held = false;
    for (int i = 0; i < count && !held; i++) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad)
            continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !held; b++)
            held = SDL_GetGamepadButton(pad, (SDL_GamepadButton)b);
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && !held; a++) {
            const int v = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)a);
            held = v > 16000 || v < -16000;
        }
    }
    SDL_free(ids);
    return held;
}

void SetGamepadNav(bool enabled)
{
    ImGuiIO& io = ImGui::GetIO();
    if (enabled)
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
}

void KeyboardLabel(const PortBinding& b, char* out, size_t size)
{
    const char* name = b.key != SDL_SCANCODE_UNKNOWN ? SDL_GetScancodeName(b.key) : "";
    snprintf(out, size, "%s", (name && *name) ? name : "-");
}

const char* ButtonName(SDL_GamepadButton button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "Abajo (A/Cruz)";
    case SDL_GAMEPAD_BUTTON_EAST: return "Derecha (B/Círculo)";
    case SDL_GAMEPAD_BUTTON_WEST: return "Izquierda (X/Cuadrado)";
    case SDL_GAMEPAD_BUTTON_NORTH: return "Arriba (Y/Triángulo)";
    case SDL_GAMEPAD_BUTTON_BACK: return "Select";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Guía";
    case SDL_GAMEPAD_BUTTON_START: return "Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "L3";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "R3";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "L1";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "R1";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "Cruceta ^";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "Cruceta v";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "Cruceta <";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "Cruceta >";
    default: {
        const char* name = SDL_GetGamepadStringForButton(button);
        return name ? name : "?";
    }
    }
}

const char* AxisName(SDL_GamepadAxis axis, int sign)
{
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return sign < 0 ? "Stick <" : "Stick >";
    case SDL_GAMEPAD_AXIS_LEFTY: return sign < 0 ? "Stick ^" : "Stick v";
    case SDL_GAMEPAD_AXIS_RIGHTX: return sign < 0 ? "Stick D <" : "Stick D >";
    case SDL_GAMEPAD_AXIS_RIGHTY: return sign < 0 ? "Stick D ^" : "Stick D v";
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "L2";
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "R2";
    default: return "?";
    }
}

void GamepadLabel(const PortBinding& b, char* out, size_t size)
{
    const bool hasButton = b.button != SDL_GAMEPAD_BUTTON_INVALID;
    const bool hasAxis = b.axis != SDL_GAMEPAD_AXIS_INVALID;
    if (hasButton && hasAxis)
        snprintf(out, size, "%s, %s", ButtonName(b.button), AxisName(b.axis, b.axisSign));
    else if (hasButton)
        snprintf(out, size, "%s", ButtonName(b.button));
    else if (hasAxis)
        snprintf(out, size, "%s", AxisName(b.axis, b.axisSign));
    else
        snprintf(out, size, "-");
}

void Close()
{
    sOpen = false;
    sEditing = false;
    sDragging = false;
    sCapture = CAPTURE_NONE;
}

void StartCapture(int input, CaptureDevice device)
{
    sCapture = device;
    sCaptureInput = input;
    sCaptureStartNs = SDL_GetTicksNS();
    // The gamepad must not also navigate the menu with the button it binds.
    SetGamepadNav(false);
    sNavHeld = true;
}

void FinishCapture()
{
    sCapture = CAPTURE_NONE;
    sHost.applyConfig();
}

// ---- pages ------------------------------------------------------------------------

void PageStates()
{
    const float thumbH = 64.0f * sScale;
    const float thumbW = thumbH * 1.5f;

    ImGui::TextWrapped("Guarda la partida en cualquier momento. F5 guarda y F8 carga la última ranura usada.");
    ImGui::Spacing();
    for (int slot = 1; slot <= PORT_STATE_SLOTS; slot++) {
        char info[64] = "";
        SDL_Texture* thumb = NULL;
        const bool used = sHost.stateInfo(slot, info, sizeof(info), &thumb);

        ImGui::PushID(slot);
        ImGui::BeginGroup();
        if (thumb)
            ImGui::Image((ImTextureID)(intptr_t)thumb, ImVec2(thumbW, thumbH));
        else
            ImGui::Dummy(ImVec2(thumbW, thumbH));
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("Ranura %d", slot);
        ImGui::TextDisabled("%s", used ? info : "Vacía");
        if (ImGui::Button("Guardar")) {
            char error[128];
            char msg[160];
            if (sHost.saveState(slot, error, sizeof(error)))
                snprintf(msg, sizeof(msg), "Guardado en la ranura %d.", slot);
            else
                snprintf(msg, sizeof(msg), "No se pudo guardar: %s", error);
            SetStatus(msg);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!used);
        if (ImGui::Button("Cargar")) {
            char error[128];
            if (sHost.loadState(slot, error, sizeof(error))) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Estado %d cargado", slot);
                sHost.toast(msg);
                Close();
            } else {
                char msg[160];
                snprintf(msg, sizeof(msg), "No se pudo cargar: %s", error);
                SetStatus(msg);
            }
        }
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::Spacing();
    }
}

void PageControls()
{
    PortConfig& cfg = Config();

    ImGui::TextWrapped("Elige una casilla y pulsa la tecla o el botón nuevo.");
    if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Botón", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Teclado", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Mando", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < PORT_INPUT_COUNT; i++) {
            char label[96];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kInputLabels[i]);
            ImGui::TableNextColumn();
            KeyboardLabel(cfg.bindings[i], label, sizeof(label));
            if (ImGui::Button(label, ImVec2(-FLT_MIN, 0)))
                StartCapture(i, CAPTURE_KEYBOARD);
            ImGui::TableNextColumn();
            GamepadLabel(cfg.bindings[i], label, sizeof(label));
            if (ImGui::Button(label, ImVec2(-FLT_MIN, 0)))
                StartCapture(i, CAPTURE_GAMEPAD);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::SliderInt("Zona muerta del stick", &cfg.stickDeadzone, 5, 95, "%d %%"))
        sHost.applyConfig();
    if (ImGui::Button("Restablecer controles")) {
        PortConfig_DefaultBindings(&cfg);
        sHost.applyConfig();
    }
}

void PageVideo()
{
    PortConfig& cfg = Config();
    bool changed = false;
    static const char* const kFilters[PORT_FILTER_COUNT] = { "Ninguno", "LCD (rejilla de píxeles)", "CRT (líneas de barrido)" };

#ifndef __ANDROID__
    changed |= ImGui::Checkbox("Pantalla completa", &cfg.fullscreen);
#endif
    changed |= ImGui::Checkbox("Escalado entero (píxeles idénticos)", &cfg.integerScaling);
    changed |= ImGui::Checkbox("Filtro suave (bilineal)", &cfg.smoothScaling);
    changed |= ImGui::Combo("Efecto", &cfg.filter, kFilters, PORT_FILTER_COUNT);
    static const char* const kWide[PORT_WIDE_COUNT] = { "No (como la GBA)", "Automática",
                                                         "16:10", "16:9", "2:1" };
    changed |= ImGui::Combo("Pantalla panorámica", &cfg.widescreen, kWide, PORT_WIDE_COUNT);
    Hint("Muestra más escenario a los lados en lugar de bandas negras (Automática: tan ancha como la "
         "pantalla). El juego no cambia: solo se ve más. "
         "Los menús, el mapa y las cinemáticas siguen en 3:2 en el centro, y en las salas más estrechas "
         "que la pantalla quedan bandas negras.");
    changed |= ImGui::Checkbox("Sincronización vertical (VSync)", &cfg.vsync);
    Hint("Con VSync la imagen no se corta, pero puede añadir algo de retraso.");

    ImGui::Spacing();
    ImGui::SeparatorText("Fluidez");
    changed |= ImGui::Checkbox("Ajustar la velocidad a la pantalla", &cfg.syncToDisplay);
    Hint("En pantallas de 60, 120 o 240 Hz el juego va un 0,46 % más rápido para coincidir con la "
         "pantalla: sin tirones. El sonido se ajusta solo. Activa VSync.");
    changed |= ImGui::Checkbox("Mezclar fotogramas (90, 144 Hz...)", &cfg.frameBlend);
    Hint("Si la pantalla no va a 60 Hz ni a un múltiplo, cada refresco mezcla los dos últimos fotogramas "
         "para suavizar el movimiento (deja una ligera estela). Activa VSync.");
    if (sHost.syncStatus) {
        char status[160];
        sHost.syncStatus(status, sizeof(status));
        Hint(status);
    }
    if (changed)
        sHost.applyConfig();
}

void PageSoundSpeed()
{
    PortConfig& cfg = Config();
    static const char* const kModes[] = { "Mantener pulsado", "Activar / desactivar" };
    int mode = cfg.fastForwardToggle ? 1 : 0;

    if (ImGui::SliderInt("Volumen", &cfg.volume, 0, 100, "%d %%"))
        sHost.applyConfig();
    ImGui::Spacing();
    ImGui::SeparatorText("Avance rápido");
    if (ImGui::SliderInt("Velocidad", &cfg.fastForwardSpeed, 2, 10, "x%d"))
        sHost.applyConfig();
    if (ImGui::Combo("Botón", &mode, kModes, 2)) {
        cfg.fastForwardToggle = mode == 1;
        sHost.applyConfig();
    }
    char key[64], pad[96];
    KeyboardLabel(cfg.bindings[PORT_INPUT_FAST_FORWARD], key, sizeof(key));
    GamepadLabel(cfg.bindings[PORT_INPUT_FAST_FORWARD], pad, sizeof(pad));
    char hint[256];
    snprintf(hint, sizeof(hint), "Tecla: %s · Mando: %s (se cambian en Controles).", key, pad);
    Hint(hint);
}

// Rumble and the second screen (AYN Thor's lower display, a window on PC).
void PageExtras()
{
    PortConfig& cfg = Config();
    static const char* const kScreen2[PORT_SCREEN2_COUNT] = { "Desactivada", "Mapa de la zona", "Estado de Samus" };
    bool changed = false;

    ImGui::SeparatorText("Vibración");
    changed |= ImGui::Checkbox("Vibración del mando", &cfg.rumbleGamepad);
#ifdef __ANDROID__
    changed |= ImGui::Checkbox("Vibración del dispositivo", &cfg.rumbleDevice);
#endif
    changed |= ImGui::SliderInt("Intensidad", &cfg.rumbleStrength, 0, 100, "%d %%");
    Hint("Al recibir daño, con las bombas y las bombas de poder, los temblores de los jefes, "
         "las caídas desde muy alto y al recoger objetos.");

    ImGui::Spacing();
    ImGui::SeparatorText("Segunda pantalla");
    changed |= ImGui::Combo("Mostrar", &cfg.secondScreen, kScreen2, PORT_SCREEN2_COUNT);
#ifdef __ANDROID__
    Hint("En dispositivos con dos pantallas (AYN Thor), la de abajo muestra el mapa con la posición "
         "de Samus o su energía, munición y equipo. Con una sola pantalla no hace nada.");
#else
    Hint("Abre una segunda ventana (en el segundo monitor si lo hay) con el mapa de la zona o el estado de Samus.");
#endif
    if (changed)
        sHost.applyConfig();
}

void PageTouch()
{
    PortConfig& cfg = Config();
    static const char* const kModes[] = { "Automático", "Siempre", "Nunca" };

    if (ImGui::Combo("Mostrar", &cfg.touchControls, kModes, 3))
        sHost.applyConfig();
    Hint("Automático: aparece al tocar la pantalla y se oculta al usar un mando.");
    if (ImGui::SliderInt("Opacidad", &cfg.touchOpacity, 0, 100, "%d %%"))
        sHost.applyConfig();
    ImGui::Spacing();
    if (ImGui::Button("Editar posición y tamaño...")) {
        sEditing = true;
        sEditSelected = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Restablecer disposición")) {
        PortConfig_DefaultTouchLayout(&cfg);
        sHost.applyConfig();
    }
}

// ---- touch layout editor ---------------------------------------------------------

int TouchButtonAt(float x, float y)
{
    int best = -1;
    float bestDist = 0.0f;
    for (int i = 0; i < PORT_TOUCH_BUTTON_COUNT; i++) {
        float cx, cy, r;
        sHost.touchButtonCircle(i, &cx, &cy, &r);
        const float d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
        if (d <= r * r * 1.2f && (best < 0 || d < bestDist)) {
            best = i;
            bestDist = d;
        }
    }
    return best;
}

void EditorEvent(const SDL_Event* e)
{
    SDL_Event ev = *e;
    // Window coordinates -> render pixels (they differ on high-DPI screens).
    SDL_ConvertEventToRenderCoordinates(sHost.renderer, &ev);
    if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
        const float scale = ImGui::GetIO().DisplayFramebufferScale.x;
        const float px = ev.button.x / scale, py = ev.button.y / scale;
        if (px >= sEditorMin.x && px < sEditorMax.x && py >= sEditorMin.y && py < sEditorMax.y)
            return; // a click on the panel
        const int hit = TouchButtonAt(ev.button.x, ev.button.y);
        if (hit >= 0) {
            float cx, cy, r;
            sHost.touchButtonCircle(hit, &cx, &cy, &r);
            sEditSelected = hit;
            sDragging = true;
            sDragDx = cx - ev.button.x;
            sDragDy = cy - ev.button.y;
        }
    } else if (ev.type == SDL_EVENT_MOUSE_MOTION && sDragging && sEditSelected >= 0) {
        sHost.moveTouchButton(sEditSelected, ev.motion.x + sDragDx, ev.motion.y + sDragDy);
    } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) {
        sDragging = false;
    }
}

void EditorWindow()
{
    PortConfig& cfg = Config();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    static const char* const kInputs[PORT_INPUT_GBA_COUNT] = {
        "A", "B", "Select", "Start", "Derecha", "Izquierda", "Arriba", "Abajo", "R", "L",
    };

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.22f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("Editar mando táctil", NULL,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Arrastra los botones para moverlos.");
    if (sEditSelected >= 0) {
        PortTouchButton& b = cfg.touch[sEditSelected];
        float size = b.r * 100.0f;
        ImGui::SetNextItemWidth(160.0f * sScale);
        if (ImGui::SliderFloat("Tamaño", &size, 2.0f, 25.0f, "%.1f"))
            b.r = size / 100.0f;
        if (b.input >= 0) {
            ImGui::SetNextItemWidth(160.0f * sScale);
            ImGui::Combo("Función", &b.input, kInputs, PORT_INPUT_GBA_COUNT);
        } else {
            ImGui::TextDisabled("Este botón abre el menú.");
        }
    } else {
        ImGui::TextDisabled("Toca un botón para cambiar su tamaño o su función.");
    }
    ImGui::SetNextItemWidth(160.0f * sScale);
    ImGui::SliderInt("Opacidad", &cfg.touchOpacity, 5, 100, "%d %%");
    if (ImGui::Button("Hecho")) {
        sEditing = false;
        sHost.applyConfig();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restablecer")) {
        PortConfig_DefaultTouchLayout(&cfg);
        sEditSelected = -1;
    }
    sEditorMin = ImGui::GetWindowPos();
    sEditorMax = ImVec2(sEditorMin.x + ImGui::GetWindowSize().x, sEditorMin.y + ImGui::GetWindowSize().y);
    ImGui::End();
}

// ---- main window -------------------------------------------------------------------

void CaptureWindow()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("##capture", NULL,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoSavedSettings);
    if (sCapture == CAPTURE_KEYBOARD) {
        ImGui::Text("Pulsa una tecla para «%s».", kInputLabels[sCaptureInput]);
        ImGui::TextDisabled("Esc cancela, Supr la deja sin tecla.");
    } else {
        const int left = 5 - (int)((SDL_GetTicksNS() - sCaptureStartNs) / 1000000000ull);
        ImGui::Text("Pulsa un botón o mueve un stick para «%s».", kInputLabels[sCaptureInput]);
        ImGui::TextDisabled("Se cancela en %d s.", left > 0 ? left : 0);
    }
    if (ImGui::Button("Cancelar"))
        sCapture = CAPTURE_NONE;
    ImGui::SameLine();
    if (ImGui::Button("Quitar")) {
        PortBinding& b = Config().bindings[sCaptureInput];
        if (sCapture == CAPTURE_KEYBOARD) {
            b.key = SDL_SCANCODE_UNKNOWN;
        } else {
            b.button = SDL_GAMEPAD_BUTTON_INVALID;
            b.axis = SDL_GAMEPAD_AXIS_INVALID;
            b.axisSign = 0;
        }
        FinishCapture();
    }
    ImGui::End();
}

void MainWindow()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(SDL_min(vp->WorkSize.x * 0.94f, 760.0f * sScale), vp->WorkSize.y * 0.92f);

    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("Metroid Fusion", NULL,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings);

    // Left: sections. Right: the selected page.
    ImGui::BeginChild("nav", ImVec2(170.0f * sScale, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    if (ImGui::Selectable("Continuar", false))
        Close();
    ImGui::Separator();
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (ImGui::Selectable(kPageNames[i], sPage == i)) {
            sPage = i;
            sStatus[0] = '\0';
        }
    }
    ImGui::Separator();
    if (ImGui::Selectable("Salir del juego", false)) {
        Close();
        sHost.quit();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::SeparatorText(kPageNames[sPage]);
    switch (sPage) {
    case PAGE_STATES: PageStates(); break;
    case PAGE_CONTROLS: PageControls(); break;
    case PAGE_VIDEO: PageVideo(); break;
    case PAGE_SOUND_SPEED: PageSoundSpeed(); break;
    case PAGE_TOUCH: PageTouch(); break;
    case PAGE_EXTRAS: PageExtras(); break;
    default: break;
    }
    if (sStatus[0]) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", sStatus);
    }
    ImGui::PopItemWidth();
    ImGui::EndChild();
    ImGui::End();
}

// Returns true if the rebinding capture used the event.
bool CaptureEvent(const SDL_Event* e)
{
    PortBinding& b = Config().bindings[sCaptureInput];

    if (sCapture == CAPTURE_KEYBOARD) {
        if (e->type != SDL_EVENT_KEY_DOWN || e->key.repeat)
            return e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP;
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) {
            sCapture = CAPTURE_NONE;
        } else if (e->key.scancode == SDL_SCANCODE_DELETE) {
            b.key = SDL_SCANCODE_UNKNOWN;
            FinishCapture();
        } else {
            // A key can only do one thing.
            for (int i = 0; i < PORT_INPUT_COUNT; i++) {
                if (Config().bindings[i].key == e->key.scancode)
                    Config().bindings[i].key = SDL_SCANCODE_UNKNOWN;
            }
            b.key = e->key.scancode;
            FinishCapture();
        }
        return true;
    }

    if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_ESCAPE) {
        sCapture = CAPTURE_NONE;
        return true;
    }
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        b.button = (SDL_GamepadButton)e->gbutton.button;
        b.axis = SDL_GAMEPAD_AXIS_INVALID;
        b.axisSign = 0;
        FinishCapture();
        return true;
    }
    if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e->gaxis.value > 24000 || e->gaxis.value < -24000)) {
        b.button = SDL_GAMEPAD_BUTTON_INVALID;
        b.axis = (SDL_GamepadAxis)e->gaxis.axis;
        b.axisSign = e->gaxis.value > 0 ? 1 : -1;
        FinishCapture();
        return true;
    }
    return e->type == SDL_EVENT_GAMEPAD_BUTTON_UP || e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
}

} // namespace

// ---- interface ------------------------------------------------------------------------

bool PortMenu_Init(const PortMenuHost* host)
{
    sHost = *host;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 16.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.06f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.55f, 0.20f, 0.10f, 0.6f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.75f, 0.30f, 0.12f, 0.8f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.85f, 0.35f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.30f, 0.45f, 0.8f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.75f, 0.30f, 0.12f, 0.9f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.85f, 0.35f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_NavCursor] = ImVec4(1.0f, 0.75f, 0.2f, 1.0f);
    sBaseStyle = style;

    if (!ImGui_ImplSDL3_InitForSDLRenderer(sHost.window, sHost.renderer) ||
        !ImGui_ImplSDLRenderer3_Init(sHost.renderer)) {
        ImGui::DestroyContext();
        return false;
    }
    sReady = true;
    return true;
}

void PortMenu_Shutdown(void)
{
    if (!sReady)
        return;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    sReady = false;
}

void PortMenu_Open(void)
{
    sOpen = true;
    sEditing = false;
    sCapture = CAPTURE_NONE;
    sStatus[0] = '\0';
    // Buttons used to open the menu are probably still held.
    SetGamepadNav(false);
    sNavHeld = true;
}

bool PortMenu_IsOpen(void)
{
    return sOpen;
}

bool PortMenu_EditingTouchPad(void)
{
    return sOpen && sEditing;
}

void PortMenu_HandleEvent(const SDL_Event* e)
{
    if (!sOpen)
        return;
    if (sCapture != CAPTURE_NONE && CaptureEvent(e))
        return;

    if (sCapture == CAPTURE_NONE) {
        const bool back = (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&
                           (e->key.key == SDLK_ESCAPE || e->key.key == SDLK_AC_BACK)) ||
                          (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START);
        if (back) {
            if (sEditing) {
                sEditing = false;
                sHost.applyConfig();
            } else {
                Close();
            }
            return;
        }
    }
    if (sEditing)
        EditorEvent(e);
    ImGui_ImplSDL3_ProcessEvent(e);
}

void PortMenu_Render(void)
{
    if (!sOpen)
        return;
    if (sNavHeld && !AnyGamepadInputHeld()) {
        sNavHeld = false;
        SetGamepadNav(true);
    }
    if (sCapture == CAPTURE_GAMEPAD && SDL_GetTicksNS() - sCaptureStartNs > 5000000000ull)
        sCapture = CAPTURE_NONE;

    UpdateScale();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (sEditing)
        EditorWindow();
    else if (sCapture != CAPTURE_NONE)
        CaptureWindow();
    else
        MainWindow();
    ImGui::Render();

    if (sEditing)
        sHost.drawTouchPad(sEditSelected);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), sHost.renderer);
}
