/* version.h - the tank's release number, the one place it lives (2026-09-29).
 *
 * Strato: "officially the tank should have a true version number so people
 * can easily understand what version they are on ... I would still consider
 * this alpha, pre 1.0". Everything shipped from the 09-11 launch to 09-29 is
 * the 0.1 series (the changelog keeps its git ids); 0.2.0 is the first
 * release under the batched schedule.
 *
 *   0.MINOR.0   a batched release (the number people talk about)
 *   0.x.PATCH   a fix that has to ship between releases
 *   1.0         Strato's call: stable saves, the core loop settled
 *
 * Bump it in the release commit, and write the changelog entry
 * (site/changelog.json) with the same number. The build id - the git
 * describe of the tree it was built from - rides beside it everywhere
 * (version_port_string), so a report still pins the exact build. The save
 * records PT_RELEASE_NUM of the build that wrote it (progression.c). */
#ifndef VERSION_H
#define VERSION_H

#define PT_RELEASE_MAJOR 0
#define PT_RELEASE_MINOR 2
#define PT_RELEASE_PATCH 0
#define PT_RELEASE       "0.2.0"
#define PT_RELEASE_STAGE "alpha"               /* pre-1.0: shown beside the number */
#define PT_RELEASE_NUM   ((PT_RELEASE_MAJOR << 16) | (PT_RELEASE_MINOR << 8) | PT_RELEASE_PATCH)

#endif
