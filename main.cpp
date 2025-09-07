#include <SDL.h>
#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <shobjidl.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

namespace fs = std::filesystem;

static int WIDTH = 640;
static int HEIGHT = 360;
static int STRIDE = WIDTH; // NV12 stride

std::string selectFolder() {
    IFileDialog* pfd = nullptr;
    std::string result;
    if (SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_PPV_ARGS(&pfd)))) {
            DWORD options;
            pfd->GetOptions(&options);
            pfd->SetOptions(options | FOS_PICKFOLDERS);
            if (SUCCEEDED(pfd->Show(NULL))) {
                IShellItem* psi;
                if (SUCCEEDED(pfd->GetResult(&psi))) {
                    PWSTR pszPath;
                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                        char buf[MAX_PATH];
                        size_t converted = 0;
                        wcstombs_s(&converted, buf, MAX_PATH, pszPath, _TRUNCATE);
                        result = buf;
                        CoTaskMemFree(pszPath);
                    }
                    psi->Release();
                }
            }
            pfd->Release();
        }
        CoUninitialize();
    }
    return result;
}

// 读取 NV12 文件 -> SDL_Texture
SDL_Texture* loadNV12toTexture(SDL_Renderer* renderer, const std::string& path, int width, int height, int stride) {
    // 计算需要的帧大小，并检查文件大小是否足够
    size_t frameSize = (size_t)stride * (size_t)height + (size_t)stride * (size_t)(height / 2);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open file: " << path << "\n";
        return nullptr;
    }

    std::vector<uint8_t> buffer(frameSize);
    file.read((char*)buffer.data(), frameSize);

    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_NV12,
        SDL_TEXTUREACCESS_STREAMING,
        width, height
    );
    if (!texture) return nullptr;

    const uint8_t* yStart = buffer.data();
    const uint8_t* uvStart = buffer.data() + stride * height;

    if (SDL_UpdateNVTexture(texture, NULL, yStart, stride, uvStart, stride) != 0) {
        SDL_DestroyTexture(texture);
        return nullptr;
    }
    return texture;
}

// 重新加载主图和缩略图
void reloadTextures(SDL_Renderer* renderer, std::vector<std::string>& files, int width, int height, int stride,
    SDL_Texture*& mainTexture, std::vector<SDL_Texture*>& thumbTextures, int idx) {

    if (mainTexture) { SDL_DestroyTexture(mainTexture); mainTexture = nullptr; }

    for (auto& tex : thumbTextures) {
        if (tex) SDL_DestroyTexture(tex);
        tex = nullptr;
    }

    mainTexture = loadNV12toTexture(renderer, files[idx], width, height, stride); // 使用当前 idx
    for (size_t i = 0; i < files.size(); ++i) {
        thumbTextures[i] = loadNV12toTexture(renderer, files[i], width, height, stride);
    }
}


int main(int argc, char* argv[]) {
    std::string folder;

    // 参数检查
    if (argc == 2 && std::string(argv[1]) == "-h") {
        std::cout << "Usage:\n";
        std::cout << "  SimpleYUVPlayer.exe WIDTH HEIGHT STRIDE DIRECTORY\n";
        std::cout << "Example:\n";
        std::cout << "  SimpleYUVPlayer.exe 1920 1080 1920 D:\\yuv\n";
        return 0;
    }

    // 解析命令行参数
    if (argc >= 3) {
            WIDTH = std::stoi(argv[1]);
            HEIGHT = std::stoi(argv[2]);
            STRIDE = WIDTH; // 默认 stride = width

            if (argc >= 4) {
                STRIDE = std::stoi(argv[3]);
            }
            if (argc >= 5) {
                folder = argv[4];
            }
            else {
                folder = "."; // 默认当前目录
            }
    }
    else {
            // 没有参数 → 弹出文件夹选择窗口
            folder = selectFolder();
            if (folder.empty()) {
                std::cout << "No folder selected.\n";
                return 0;
            }
    }

    //打印确认
    std::cout << "Width=" << WIDTH << " Height=" << HEIGHT
        << " Stride=" << STRIDE
        << " Folder=" << folder << std::endl;

    //遍历文件
    std::vector<std::string> files;
    for (auto& p : fs::directory_iterator(folder)) {
        if (p.path().extension() == ".YUV420NV12") files.push_back(p.path().string());
    }
    std::sort(files.begin(), files.end(),
        [](const std::string& a, const std::string& b) {
            std::wstring wa(a.begin(), a.end());
            std::wstring wb(b.begin(), b.end());
            return StrCmpLogicalW(wa.c_str(), wb.c_str()) < 0;
        });
    if (files.empty()) { std::cout << "No .YUV files found.\n"; return 0; }

    int defaultWinW = 900;
    int defaultWinH = 650;
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow(
        "NV12 Viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        defaultWinW + 300, defaultWinH + 150,
        SDL_WINDOW_RESIZABLE
    );
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    int idx = 0;
    int prevIdx = idx;
    SDL_Texture* texture = nullptr;
    bool quit = false;

    std::vector<SDL_Texture*> thumbTextures(files.size(), nullptr);
    reloadTextures(renderer, files, WIDTH, HEIGHT, STRIDE, texture, thumbTextures, idx);

    const int thumbMargin = 5;

    int dragStartX = 0;
    float scrollXStart = 0.0f;
    bool dragging = false;

    while (!quit) {
        int windowW, windowH;
        SDL_GetWindowSize(window, &windowW, &windowH);

        float bottomStartY = windowH * 4.0f / 5.0f;
        int imgAreaW = windowW * 3 / 4;
        int imgAreaH = (int)bottomStartY;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) quit = true;
            else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) quit = true;
                else if (e.key.keysym.sym == SDLK_RIGHT) {
                    idx = (idx + 1) % files.size();
                    if (texture) SDL_DestroyTexture(texture);
                    texture = loadNV12toTexture(renderer, files[idx], WIDTH, HEIGHT, STRIDE);
                }
                else if (e.key.keysym.sym == SDLK_LEFT) {
                    idx = (idx - 1 + files.size()) % files.size();
                    if (texture) SDL_DestroyTexture(texture);
                    texture = loadNV12toTexture(renderer, files[idx], WIDTH, HEIGHT, STRIDE);
                }
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // 右侧控制面板
        ImGui::SetNextWindowPos(ImVec2(imgAreaW, 0));
        ImGui::SetNextWindowSize(ImVec2(windowW - imgAreaW, imgAreaH));
        ImGui::Begin("Control Panel", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

        int oldWidth = WIDTH, oldHeight = HEIGHT, oldStride = STRIDE;
        ImGui::InputInt("Width", &WIDTH);
        ImGui::InputInt("Height", &HEIGHT);
        ImGui::InputInt("Stride", &STRIDE);

        if (WIDTH < 1) WIDTH = 1;
        if (WIDTH > 10000) WIDTH = 10000;
        if (HEIGHT < 1) HEIGHT = 1;
        if (HEIGHT > 10000) HEIGHT = 10000;
        if (STRIDE < 1) STRIDE = 1;
        if (STRIDE > 10000) STRIDE = 10000;

        ImGui::Text("Index: %d / %d", idx + 1, (int)files.size());
        if (ImGui::Button("Prev")) {
            idx = (idx - 1 + files.size()) % files.size();
            if (texture) SDL_DestroyTexture(texture);
            texture = loadNV12toTexture(renderer, files[idx], WIDTH, HEIGHT, STRIDE);
        }
        ImGui::SameLine();
        if (ImGui::Button("Next")) {
            idx = (idx + 1) % files.size();
            if (texture) SDL_DestroyTexture(texture);
            texture = loadNV12toTexture(renderer, files[idx], WIDTH, HEIGHT, STRIDE);
        }
        ImGui::End();

        // 如果 Width/Height/Stride 改变，重新加载主图和缩略图
        if (WIDTH != oldWidth || HEIGHT != oldHeight || STRIDE != oldStride) {
            reloadTextures(renderer, files, WIDTH, HEIGHT, STRIDE, texture, thumbTextures, idx);
        }

        // 缩略图条
        ImGui::SetNextWindowPos(ImVec2(0.0f, bottomStartY));
        ImGui::SetNextWindowSize(ImVec2(imgAreaW, windowH - bottomStartY));
        ImGui::Begin("ThumbnailBarWindow", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);

        ImGui::BeginChild("ThumbScrollRegion", ImVec2(0, 0), false,
            ImGuiWindowFlags_HorizontalScrollbar);

        ImGuiIO& io = ImGui::GetIO();
        float thumbWidth = 160.0f;

        for (int i = 0; i < files.size(); ++i) {
            SDL_Texture* thumbTex = thumbTextures[i];
            if (!thumbTex) continue;

            ImGui::PushID(i);
            ImGui::BeginGroup();

            float thumbW = 160;
            float thumbH = thumbW * HEIGHT / WIDTH;

            ImGui::Image((ImTextureID)thumbTex, ImVec2(thumbW, thumbH));
            ImVec2 thumbPos = ImGui::GetItemRectMin();
            ImVec2 thumbSize = ImGui::GetItemRectSize();

            // 双击选中
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                idx = i;
                if (texture) SDL_DestroyTexture(texture);
                texture = loadNV12toTexture(renderer, files[idx], WIDTH, HEIGHT, STRIDE);
            }

            // 红框：当前主图
            if (i == idx) {
                ImGui::GetWindowDrawList()->AddRect(
                    thumbPos,
                    ImVec2(thumbPos.x + thumbSize.x, thumbPos.y + thumbSize.y),
                    IM_COL32(255, 0, 0, 255), 0.0f, 0, 2.0f
                );
            }

            // 黄色预选框
            if (ImGui::IsItemHovered()) {
                ImGui::GetWindowDrawList()->AddRect(
                    thumbPos,
                    ImVec2(thumbPos.x + thumbSize.x, thumbPos.y + thumbSize.y),
                    IM_COL32(255, 255, 0, 128), 0.0f, 0, 2.0f
                );
            }

            // 序号
            ImGui::Text("%d", i + 1);

            ImGui::EndGroup();
            ImGui::SameLine(0, thumbMargin);
            ImGui::PopID();
        }

        // 鼠标拖动逻辑
        if (ImGui::IsWindowHovered()) {
            if (ImGui::IsMouseDown(0)) {
                if (!dragging) {
                    dragging = true;
                    dragStartX = io.MousePos.x;
                    scrollXStart = ImGui::GetScrollX();
                }
                else {
                    float delta = io.MousePos.x - dragStartX;
                    ImGui::SetScrollX(scrollXStart - delta);
                }
            }
            else {
                dragging = false;
            }
        }

        // ⭐ 更新 idx 后，让红框居中
        if (idx != prevIdx) {
            float targetCenter = idx * (thumbWidth + thumbMargin) + thumbWidth * 0.5f;
            float windowWidth = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
            float scrollX = targetCenter - windowWidth * 0.5f;
            if (scrollX < 0.0f) scrollX = 0.0f;
            ImGui::SetScrollX(scrollX);

            prevIdx = idx;
        }


        ImGui::EndChild();
        ImGui::End();

        // 文件名显示
        float filenameWinHeight = 30.0f;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(imgAreaW, filenameWinHeight));
        ImGui::Begin("FileNameDisplay", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(fs::path(files[idx]).filename().string().c_str());
        ImGui::End();

        // 主图显示
        SDL_SetRenderDrawColor(renderer, 60, 60, 60, 255);
        SDL_RenderClear(renderer);
        if (texture) {
            float scaleX = float(imgAreaW) / float(WIDTH);
            float scaleY = float(imgAreaH) / float(HEIGHT);
            float scale = (scaleX < scaleY) ? scaleX : scaleY;

            SDL_Rect dstRect;
            dstRect.w = int(WIDTH * scale);
            dstRect.h = int(HEIGHT * scale);
            dstRect.x = (imgAreaW - dstRect.w) / 2;
            dstRect.y = int(filenameWinHeight) + (imgAreaH - int(filenameWinHeight) - dstRect.h) / 2;

            SDL_RenderCopy(renderer, texture, NULL, &dstRect);
        }

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);

        SDL_RenderPresent(renderer);
        SDL_Delay(30);
    }

    if (texture) SDL_DestroyTexture(texture);
    for (auto tex : thumbTextures) if (tex) SDL_DestroyTexture(tex);

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
