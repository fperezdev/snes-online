#pragma once

#include "snesonline/AppConfig.h"

namespace snesonline {

// Opens a modal Windows dialog to edit configuration.
// Returns true if the user clicked OK (and cfg contains updated values).
bool showConfigDialog(AppConfig& cfg);

} // namespace snesonline
