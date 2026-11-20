// Snapshot-based undo/redo of the whole document.
#pragma once

#include <ska/ska.h>

#include <cstddef>
#include <optional>
#include <vector>

class History {
public:
    // Discrete op: call BEFORE mutating; clears redo.
    void push(const ska::Document& before);
    // Continuous edit start; no-op if one is already pending.
    void begin(const ska::Document& before);
    // Pushes the pending snapshot if changed; clears pending.
    void end(bool changed);
    // Swaps doc with the top of undo and pushes the current doc to redo.
    bool undo(ska::Document& doc);
    bool redo(ska::Document& doc);
    bool canUndo() const;
    bool canRedo() const;
    void clear();
    // Snapshot taken by begin() while a continuous edit is open, else nullptr.
    const ska::Document* pending() const { return pending_ ? &*pending_ : nullptr; }

private:
    std::vector<ska::Document> undo_, redo_;
    std::optional<ska::Document> pending_;
    static constexpr size_t kLimit = 200;
};
