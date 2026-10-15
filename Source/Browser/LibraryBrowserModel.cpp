#include "LibraryBrowserModel.h"
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace konos {

namespace {

std::string toLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

} // namespace

LibraryBrowserModel::LibraryBrowserModel() {
    resetToFactoryDefault();
}

void LibraryBrowserModel::resetToFactoryDefault() {
    allItems_.clear();
    allItems_.push_back(LibraryItem{"Factory Electric Grand (C4)", "Factory://ElectricGrand", "Factory", true, false, false});
    allItems_.push_back(LibraryItem{"Factory Warm Analog Pad",     "Factory://WarmPad",       "Factory", true, false, false});
    allItems_.push_back(LibraryItem{"Factory Concert Harp Pluck",  "Factory://ConcertHarp",   "Factory", true, false, false});
    allItems_.push_back(LibraryItem{"Factory Chamber Strings",     "Factory://ChamberStrings","Factory", true, false, false});
    rebuildFilteredList();
}

int LibraryBrowserModel::scanDirectory(const std::string& directoryPath) {
    std::error_code ec;
    const std::filesystem::path root(directoryPath);
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return 0;
    }

    int addedCount = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string ext = toLower(entry.path().extension().string());
        std::string cat;
        if (ext == ".wav" || ext == ".wave" || ext == ".aif" || ext == ".aiff") {
            cat = "WAV/AIFF";
        } else if (ext == ".sfz") {
            cat = "SFZ";
        } else if (ext == ".sf2") {
            cat = "SF2";
        } else if (ext == ".nki" || ext == ".nkm" || ext == ".nkx") {
            cat = "Kontakt";
        } else {
            continue;
        }

        const std::string fullPath = entry.path().string();
        const bool alreadyPresent = std::any_of(allItems_.begin(), allItems_.end(),
            [&](const LibraryItem& item) { return item.fullPath == fullPath; });
        if (!alreadyPresent) {
            LibraryItem item;
            item.name       = entry.path().filename().string();
            item.fullPath   = fullPath;
            item.category   = cat;
            item.isFactory  = false;
            item.isFavorite = (favoritePaths_.count(fullPath) > 0);
            allItems_.push_back(std::move(item));
            ++addedCount;
        }
    }

    rebuildFilteredList();
    return addedCount;
}

void LibraryBrowserModel::setSearchQuery(const std::string& query) {
    searchQuery_ = query;
    rebuildFilteredList();
}

void LibraryBrowserModel::setCategoryFilter(const std::string& category) {
    categoryFilter_ = category.empty() ? "All" : category;
    rebuildFilteredList();
}

bool LibraryBrowserModel::toggleFavoriteByPath(const std::string& fullPath) {
    bool nowFav = false;
    if (favoritePaths_.count(fullPath) > 0) {
        favoritePaths_.erase(fullPath);
        nowFav = false;
    } else {
        favoritePaths_.insert(fullPath);
        nowFav = true;
    }
    for (auto& item : allItems_) {
        if (item.fullPath == fullPath) {
            item.isFavorite = nowFav;
        }
    }
    rebuildFilteredList();
    return nowFav;
}

void LibraryBrowserModel::recordRecentInstrument(const std::string& fullPath) {
    recentPaths_.erase(std::remove(recentPaths_.begin(), recentPaths_.end(), fullPath), recentPaths_.end());
    recentPaths_.insert(recentPaths_.begin(), fullPath);
    if (recentPaths_.size() > 16) {
        recentPaths_.resize(16);
    }
}

void LibraryBrowserModel::rebuildFilteredList() {
    filteredItems_.clear();
    const std::string qLower = toLower(searchQuery_);

    for (const auto& item : allItems_) {
        if (categoryFilter_ == "Favorites") {
            if (!item.isFavorite) continue;
        } else if (categoryFilter_ != "All" && item.category != categoryFilter_) {
            continue;
        }

        if (!qLower.empty()) {
            const std::string nameLower = toLower(item.name);
            const std::string catLower  = toLower(item.category);
            if (nameLower.find(qLower) == std::string::npos &&
                catLower.find(qLower) == std::string::npos) {
                continue;
            }
        }
        filteredItems_.push_back(item);
    }
}

} // namespace konos
