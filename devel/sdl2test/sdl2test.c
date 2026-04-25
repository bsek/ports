#include <SDL.h>
#include <stdio.h>

int main(int argc, char *argv[]) {
    printf("SDL2 Test Program\n");

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    printf("SDL_Init OK\n");

    /* Try software window first - no OpenGL */
    SDL_Window *window = SDL_CreateWindow(
        "SDL2 Test",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640, 480,
        0  /* no flags = software */
    );

    if (!window) {
        printf("SDL_CreateWindow (software) failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    printf("SDL_CreateWindow OK (software)\n");

    SDL_Surface *surface = SDL_GetWindowSurface(window);
    if (!surface) {
        printf("SDL_GetWindowSurface failed: %s\n", SDL_GetError());
    } else {
        printf("Window surface: %dx%d, %d bpp\n",
            surface->w, surface->h,
            surface->format->BitsPerPixel);

        /* Fill with blue */
        SDL_FillRect(surface, NULL, SDL_MapRGB(surface->format, 0, 0, 255));
        SDL_UpdateWindowSurface(window);
        printf("Drew blue screen. Waiting 3 seconds...\n");
        SDL_Delay(3000);
    }

    SDL_DestroyWindow(window);
    printf("Software window test passed.\n\n");

    /* Now try OpenGL window */
    printf("Trying OpenGL window...\n");
    SDL_Window *glwindow = SDL_CreateWindow(
        "SDL2 GL Test",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640, 480,
        SDL_WINDOW_OPENGL
    );

    if (!glwindow) {
        printf("SDL_CreateWindow (OpenGL) failed: %s\n", SDL_GetError());
    } else {
        printf("SDL_CreateWindow OK (OpenGL)\n");
        SDL_GLContext glctx = SDL_GL_CreateContext(glwindow);
        if (!glctx) {
            printf("SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        } else {
            printf("OpenGL context created OK\n");
            SDL_GL_DeleteContext(glctx);
        }
        SDL_DestroyWindow(glwindow);
    }

    SDL_Quit();
    printf("Done.\n");
    return 0;
}
