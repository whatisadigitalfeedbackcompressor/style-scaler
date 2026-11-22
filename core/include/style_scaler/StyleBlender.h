#pragma once

#include "style_scaler/StyleProfile.h"
#include "style_scaler/Types.h"

#include <optional>

namespace style_scaler {

struct BlendParams {
    double harmony = 0.0; // 0.0 = Slot A, 1.0 = Slot B
    double rhythm = 0.0;
    double voicing = 0.0;
    double melody = 0.0;
    TempoScale tempo_scale = TempoScale::Normal;
    SongSection section = SongSection::Chorus;
    std::optional<int> key_override_pc;
    std::optional<Mode> mode_override;
};

class StyleBlender {
public:
    static StyleProfile blend(const StyleProfile& slotA,
                              const StyleProfile& slotB,
                              const BlendParams& params);
};

} // namespace style_scaler
