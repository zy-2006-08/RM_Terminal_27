#pragma once

#include "domain.h"
#include <QString>
#include <set>

namespace rm_terminal {

QString freshness_text(Freshness freshness);
QString quality_text(Quality quality);
QString snapshot_text(const Snapshot& snapshot);

// Reports fields that aged into Stale since the previous call, so a field that
// stays stale logs once per transition rather than once per snapshot.
class StaleReporter {
public:
    void inspect(const Snapshot& snapshot);
private:
    std::set<QString> stale_;
};

class PresentationState {
public:
    explicit PresentationState(Snapshot snapshot) : snapshot_(std::move(snapshot)) {}
    const Snapshot& snapshot() const { return snapshot_; }
private:
    const Snapshot snapshot_;
};

}
