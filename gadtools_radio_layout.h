#ifndef MINTAMP_GADTOOLS_RADIO_LAYOUT_H
#define MINTAMP_GADTOOLS_RADIO_LAYOUT_H

/* Shorten only the results list on low-resolution public screens. */
typedef struct GadToolsRadioLayout {
    int left, top, width, height, results_height, favourites_y, buttons_y, status_y;
} GadToolsRadioLayout;
static __inline__ int gadtools_radio_layout(GadToolsRadioLayout *r,
                                            int screen_width, int screen_height,
                                            int parent_left, int parent_top)
{
    int shrink;
    if (screen_width<548 || screen_height<200) return 0;
    r->width=548; r->height=screen_height<262 ? screen_height : 262;
    shrink=262-r->height;
    r->results_height=94-shrink; r->favourites_y=188-shrink;
    r->buttons_y=210-shrink; r->status_y=234-shrink;
    r->left=parent_left+30; r->top=parent_top+30;
    if (r->left<0) r->left=0;
    if (r->top<0) r->top=0;
    if (r->left>screen_width-r->width) r->left=screen_width-r->width;
    if (r->top>screen_height-r->height) r->top=screen_height-r->height;
    return 1;
}
#endif
