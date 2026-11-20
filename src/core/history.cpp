#include "core/history.h"

#include <utility>

void History::push(const ska::Document& before) {
    undo_.push_back(before);
    if (undo_.size() > kLimit) undo_.erase(undo_.begin());
    redo_.clear();
}

void History::begin(const ska::Document& before) {
    if (!pending_) pending_ = before;
}

void History::end(bool changed) {
    if (pending_ && changed) push(*pending_);
    pending_.reset();
}

bool History::undo(ska::Document& doc) {
    if (undo_.empty()) return false;
    redo_.push_back(std::move(doc));
    doc = std::move(undo_.back());
    undo_.pop_back();
    return true;
}

bool History::redo(ska::Document& doc) {
    if (redo_.empty()) return false;
    undo_.push_back(std::move(doc));
    doc = std::move(redo_.back());
    redo_.pop_back();
    return true;
}

bool History::canUndo() const { return !undo_.empty(); }

bool History::canRedo() const { return !redo_.empty(); }

void History::clear() {
    undo_.clear();
    redo_.clear();
    pending_.reset();
}
