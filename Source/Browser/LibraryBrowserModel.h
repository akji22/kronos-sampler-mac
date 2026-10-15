#pragma once

#include <string>
#include <vector>
#include <set>
#include <cstdint>

namespace konos {

struct LibraryItem {
    std::string name{};
    std::string fullPath{};
    std::string category{}; // "Factory", "WAV/AIFF", "SFZ", "SF2", "Kontakt"
    bool        isFactory{false};
    bool        isFavorite{false};
    bool        missingSampleWarning{false};
};

/**
 * Backend Library Browser & File Management System.
 * Supports:
 * - Built-in Factory Instruments (Electric Grand, Warm Pad, Plucked Harp, Orchestral Strings)
 * - Recursive folder scanning for .wav, .aif, .aiff, .sfz, .sf2, .nki, .nkm, .nkx
 * - Real-time search query & category filtering ("All", "Factory", "WAV/AIFF", "SFZ", "SF2", "Favorites")
 * - Favorites management & Recent Instruments history
 */
class LibraryBrowserModel {
public:
    LibraryBrowserModel();

    void resetToFactoryDefault();
    int  scanDirectory(const std::string& directoryPath);

    void setSearchQuery(const std::string& query);
    const std::string& getSearchQuery() const noexcept { return searchQuery_; }

    void setCategoryFilter(const std::string& category);
    const std::string& getCategoryFilter() const noexcept { return categoryFilter_; }

    bool toggleFavoriteByPath(const std::string& fullPath);
    void recordRecentInstrument(const std::string& fullPath);

    const std::vector<LibraryItem>& getFilteredItems() const noexcept { return filteredItems_; }
    const std::vector<std::string>& getRecentPaths() const noexcept { return recentPaths_; }
    const std::set<std::string>&    getFavoritePaths() const noexcept { return favoritePaths_; }

private:
    void rebuildFilteredList();

    std::vector<LibraryItem> allItems_{};
    std::vector<LibraryItem> filteredItems_{};
    std::set<std::string>    favoritePaths_{};
    std::vector<std::string> recentPaths_{};
    std::string              searchQuery_{};
    std::string              categoryFilter_{"All"};
};

} // namespace konos
