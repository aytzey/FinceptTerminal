// src/screens/economics/panels/FinceptMacroPanel.cpp
// Fincept Macro — proprietary macro data source.
// The script fincept_macro.py does not yet exist.
// This panel shows a Coming Soon state with description of planned data.
// When fincept_macro.py is ready, implement on_fetch() and on_result() here.
#include "screens/economics/panels/FinceptMacroPanel.h"

#include "core/logging/Logger.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace fincept::screens {
namespace {

static constexpr const char* kFinceptMacroSourceId = "local";
static constexpr const char* kFinceptMacroColor = "#d97706"; // amber
} // namespace

FinceptMacroPanel::FinceptMacroPanel(QWidget* parent)
    : EconPanelBase(kFinceptMacroSourceId, kFinceptMacroColor, parent) {
    build_base_ui(this);
    // No service connection — Coming Soon panel
}

void FinceptMacroPanel::activate() {
    show_empty("Local Macro Sources\n\n"
               "Available now elsewhere in Economics:\n"
               "  · FRED\n"
               "  · BLS\n"
               "  · BEA\n"
               "  · EIA\n"
               "  · Trading Economics\n\n"
               "This panel is reserved for a unified local macro view.");
}

void FinceptMacroPanel::build_controls(QHBoxLayout* thl) {
    auto* lbl = new QLabel("LOCAL MACRO");
    lbl->setStyleSheet(ctrl_label_style() + "letter-spacing:1px;");
    thl->addWidget(lbl);
}

void FinceptMacroPanel::on_fetch() {
    show_empty("Unified local macro view is not wired yet.\n"
               "Use the existing free-source macro panels in the meantime.");
}

void FinceptMacroPanel::on_result(const QString& /*request_id*/, const services::EconomicsResult& /*result*/) {
    // No-op until fincept_macro.py is implemented
}

} // namespace fincept::screens
