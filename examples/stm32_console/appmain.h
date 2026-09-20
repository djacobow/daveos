#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Enter the application after CubeMX clock/peripheral initialization. The
// implementation owns the file-scope modules, logger, and scheduler, performs
// staged initialization, and runs the scheduler. On H563, terminal failure
// records diagnostics and resets.
void appmain(void);

#ifdef __cplusplus
}
#endif
