#include <stdbool.h>
typedef struct OH_PixelmapNative OH_PixelmapNative;
#include <multimodalinput/oh_input_manager.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void HoverAuthorized(Input_InjectionStatus status)
{
    printf("HOVER_AUTH callback=%d\n", status);
    fflush(stdout);
}

static int AuthorizeHover(void)
{
    Input_InjectionStatus status = UNAUTHORIZED;
    int32_t result = OH_Input_QueryAuthorizedStatus(&status);
    printf("HOVER_AUTH query=%d status=%d\n", result, status);
    if (status != AUTHORIZED) {
        result = OH_Input_RequestInjection(HoverAuthorized);
        printf("HOVER_AUTH request=%d\n", result);
        fflush(stdout);
    }
    for (int attempt = 0; attempt < 100 && status != AUTHORIZED; attempt++) {
        usleep(100000);
        result = OH_Input_QueryAuthorizedStatus(&status);
    }
    printf("HOVER_AUTH query=%d status=%d\n", result, status);
    fflush(stdout);
    return status == AUTHORIZED ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 4 || (argc - 1) % 3 != 0) {
        fprintf(stderr, "usage: hover-probe <x> <y> <delay-ms> [x y delay-ms ...]\n");
        return 2;
    }
    if (AuthorizeHover() != 0) return 4;
    int32_t result = INPUT_SUCCESS;
    for (int index = 1; index < argc; index += 3) {
        int delay = atoi(argv[index + 2]);
        if (delay < 0 || delay > 30000) return 2;
        usleep(delay * 1000);
        struct Input_MouseEvent *event = OH_Input_CreateMouseEvent();
        if (!event) return 3;
        OH_Input_SetMouseEventAction(event, MOUSE_ACTION_MOVE);
        OH_Input_SetMouseEventGlobalX(event, atoi(argv[index]));
        OH_Input_SetMouseEventGlobalY(event, atoi(argv[index + 1]));
        result = OH_Input_InjectMouseEventGlobal(event);
        printf("HOVER_INJECT x=%d y=%d result=%d\n", atoi(argv[index]), atoi(argv[index + 1]), result);
        int32_t display = -1;
        double x = -1, y = -1;
        int32_t location = OH_Input_GetPointerLocation(&display, &x, &y);
        printf("HOVER_LOCATION result=%d display=%d x=%.1f y=%.1f\n", location, display, x, y);
        fflush(stdout);
        OH_Input_DestroyMouseEvent(&event);
        if (result != INPUT_SUCCESS) break;
    }
    return result == INPUT_SUCCESS ? 0 : 1;
}
