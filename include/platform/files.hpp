#pragma once

#include <cstddef>
#include <filesystem>
#include <system_error>
#include <vector>

#ifdef __PSP__
// std::filesystem::recursive_directory_iterator opens each subdirectory with openat on its parent's
// descriptor, which the PSP's libc resolves against the working directory, so it fails at the first
// descent. This one opens every directory by its whole path. It is what the port's walks use of
// the standard one; the options are taken and ignored, a memory stick having no links to follow
// and no permissions to skip.
class RecursiveDirectoryIterator {
public:
    RecursiveDirectoryIterator() = default;

    RecursiveDirectoryIterator(const std::filesystem::path &root, std::filesystem::directory_options,
                               std::error_code &error) {
        Open(root, error);
    }

    explicit RecursiveDirectoryIterator(const std::filesystem::path &root) {
        std::error_code error;
        Open(root, error);
        if (error) {
            throw std::filesystem::filesystem_error("recursive directory iterator", root, error);
        }
    }

    const std::filesystem::directory_entry &operator*() const { return *stack_.back(); }
    const std::filesystem::directory_entry *operator->() const { return &*stack_.back(); }

    RecursiveDirectoryIterator &increment(std::error_code &error) {
        error.clear();
        std::error_code type_error;
        if (stack_.back()->is_directory(type_error)) {
            std::filesystem::directory_iterator child(stack_.back()->path(), error);
            if (error) {
                return *this;
            }
            if (child != std::filesystem::directory_iterator()) {
                stack_.push_back(child);
                return *this;
            }
        }
        while (!stack_.empty()) {
            stack_.back().increment(error);
            if (error || stack_.back() != std::filesystem::directory_iterator()) {
                return *this;
            }
            stack_.pop_back();
        }
        return *this;
    }

    RecursiveDirectoryIterator &operator++() {
        std::error_code error;
        increment(error);
        if (error) {
            throw std::filesystem::filesystem_error("recursive directory iterator", error);
        }
        return *this;
    }

    bool operator==(const RecursiveDirectoryIterator &other) const { return stack_.empty() && other.stack_.empty(); }

private:
    void Open(const std::filesystem::path &root, std::error_code &error) {
        std::filesystem::directory_iterator first(root, error);
        if (!error && first != std::filesystem::directory_iterator()) {
            stack_.push_back(first);
        }
    }

    std::vector<std::filesystem::directory_iterator> stack_;
};

inline RecursiveDirectoryIterator begin(RecursiveDirectoryIterator it) { return it; }
inline RecursiveDirectoryIterator end(const RecursiveDirectoryIterator &) { return {}; }
#else
using RecursiveDirectoryIterator = std::filesystem::recursive_directory_iterator;
#endif

enum class FilesResult {
    kWritten,
    kExists, // Not written: the path was taken and replacing was not allowed.
    kFailed,
};

// Writes data to path so that it is on the disk once kWritten returns: a temporary file of its own
// beside path, created exclusively, written, flushed and closed, then moved onto path with the
// system's write-through or followed by a sync of the directory. Before that move path keeps what
// it held. Without replace, a path that exists gives kExists. Removing the temporary file is best
// effort: one named <path>.<hex>.tmp is left when the process dies during the call or the removal
// fails, and can be deleted by hand.
FilesResult FilesWrite(const std::filesystem::path &path, const void *data, std::size_t size, bool replace);

// Reads up to size bytes of the regular file at path and returns how many; anything else at path,
// a directory or a FIFO, reads as nothing, without waiting on it.
std::size_t FilesRead(const std::filesystem::path &path, void *data, std::size_t size);

// Creates a directory at path, where nothing may be: kExists when anything is. Publishes with a
// write-through move on Windows; on POSIX syncs the parent. A failed sync may leave the new name.
FilesResult FilesCreateDirectory(const std::filesystem::path &path);
