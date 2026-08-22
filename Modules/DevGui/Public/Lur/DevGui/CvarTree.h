#pragma once
// Lur::DevGui::GatherCvars — the registry, arranged the way a console wants it.
//
// Promoted out of Rps::GameView's GatherAllCvars (#201) when chess needed the identical eleven lines.
//
// ---- Why this is a separate header from CategoryTree ----
// CategoryTree is deliberately generic over its leaf type with NO Core dependency, so it stays
// host-testable against a dummy leaf. This function is the opposite: it exists precisely to know
// about Lur::Core::ICVar. Putting it next door rather than inside keeps that line intact — a game
// that wants a category tree of something else still pays nothing for Core.
//
// ---- The dotted name IS the hierarchy ----
// "chess.view.square_light" nests as chess > view > square_light. There is no separate category
// field to keep in step with the name, which is the point: a CVar cannot end up filed under a
// heading that disagrees with what it is called.
//
// ---- Sorted by NAME, not by registration order ----
// Registration order is link order, which is neither stable nor meaningful, and a console whose rows
// reshuffle between builds is one you cannot navigate by memory. CategoryTree preserves leaf order
// within a node, so sorting here is what makes each section's rows come out alphabetical.
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "Lur/Core/CVar.h"

namespace Lur::DevGui {

// (category-path, cvar) pairs for every registered CVar, sorted by full name. Feed straight to
// BuildCategoryTree with the same separator.
inline std::vector<std::pair<std::string, Lur::Core::ICVar*>> GatherCvars(char Sep = '.') {
    std::vector<std::pair<std::string, Lur::Core::ICVar*>> Items;
    Lur::Core::CVarRegistry::ForEach([&](Lur::Core::ICVar* C) {
        const std::string Name = C->Name();
        const std::size_t Cut = Name.rfind(Sep);
        // No separator at all -> no category. Such a CVar becomes a ROOT leaf, which FlatList emits
        // first and never folds away — correct, because there is no header to fold it under.
        Items.emplace_back(Cut == std::string::npos ? std::string{} : Name.substr(0, Cut), C);
    });
    std::sort(Items.begin(), Items.end(), [](const auto& A, const auto& B) {
        return std::strcmp(A.second->Name(), B.second->Name()) < 0;
    });
    return Items;
}

}  // namespace Lur::DevGui
