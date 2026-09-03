#include "hal/flight_state.h"

#include <Preferences.h>
#include <esp_system.h>

namespace hal {
namespace {

// Namespace próprio na NVS, separado do contador de boot: os dois têm cadências e
// donos diferentes.
constexpr char kNamespace[] = "ele3km_ph";
constexpr char kKeyPhase[] = "phase";
constexpr char kKeyReference[] = "ref";
constexpr char kKeyLiftoff[] = "lift";
constexpr char kKeyHasRef[] = "hasref";

}  // namespace

core::PhaseRestore FlightState::restore() {
    core::PhaseRestore restore;

    // A pergunta que só a placa responde: o reset foi um power-on limpo? Se foi,
    // o núcleo NÃO reusa a referência — um voo novo não herda o zero de outro.
    restore.clean_poweron = (esp_reset_reason() == ESP_RST_POWERON);

    Preferences prefs;
    if (!prefs.begin(kNamespace, /*readOnly=*/true)) {
        return restore;  // nada salvo ainda; have_saved fica falso
    }
    if (prefs.isKey(kKeyPhase)) {
        restore.have_saved         = true;
        restore.saved.phase        = static_cast<core::FlightPhase>(prefs.getUChar(kKeyPhase, 0));
        restore.saved.reference_pa = prefs.getFloat(kKeyReference, 0.0f);
        restore.saved.liftoff_ms   = prefs.getUInt(kKeyLiftoff, 0);
        restore.saved.has_reference = prefs.getBool(kKeyHasRef, false);
    }
    prefs.end();
    return restore;
}

void FlightState::save(const core::PhaseSnapshot& snapshot) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
        return;  // uma falha de NVS não pode calar o link; segue sem persistir
    }
    prefs.putUChar(kKeyPhase, static_cast<uint8_t>(snapshot.phase));
    prefs.putFloat(kKeyReference, snapshot.reference_pa);
    prefs.putUInt(kKeyLiftoff, snapshot.liftoff_ms);
    prefs.putBool(kKeyHasRef, snapshot.has_reference);
    prefs.end();
}

}  // namespace hal
