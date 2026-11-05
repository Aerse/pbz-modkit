// houserules: see houserules.h. Stub until the hooks are in.
#include "houserules.h"
namespace houserules {
void Begin(const rules::Rules&, ue::UObject*) {}
void Tick(const rules::Rules&, ue::UObject*, float) {}
void End(ue::UObject*) {}
float OutgoingScale(ue::UObject*) { return 1.f; }
}
