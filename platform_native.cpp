#include "platform.h"
#include "tinyfiledialogs.h"

#include <cstdio>
#include <vector>

namespace Platform {

void OpenImageFile(BytesLoadedFn on_loaded)
{
    const char* filters[] = {"*.png", "*.jpg", "*.jpeg", "*.bmp"};
    const char* path = tinyfd_openFileDialog("Select Image", "", 4, filters,
                                              "Image Files", 0);
    if (!path) return; // user cancelled

    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Failed to open: %s\n", path); return; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return; }

    std::vector<unsigned char> buf((size_t)size);
    size_t nread = fread(buf.data(), 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) {
        fprintf(stderr, "Short read on: %s\n", path);
        return;
    }

    on_loaded(buf.data(), buf.size());
}

bool SaveFile(const std::string& suggested_name,
             const unsigned char* data, size_t size)
{
    const char* filters[] = {"*.png"};
    const char* path = tinyfd_saveFileDialog("Export PNG", suggested_name.c_str(),
                                              1, filters, "PNG");
    if (!path) return false; // user cancelled

    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Failed to open for write: %s\n", path); return false; }

    size_t nwritten = fwrite(data, 1, size, f);
    fclose(f);
    return nwritten == size;
}

} // namespace Platform
