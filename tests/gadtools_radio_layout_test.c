#include "gadtools_radio_layout.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    GadToolsRadioLayout r;
    int height, left, top;
    assert(!gadtools_radio_layout(&r,547,512,0,0));
    assert(!gadtools_radio_layout(&r,640,199,0,0));
    for (height=200;height<=1024;++height)
        for (left=-100;left<=800;left+=100)
            for (top=-100;top<=1100;top+=100) {
                assert(gadtools_radio_layout(&r,640,height,left,top));
                assert(r.left>=0 && r.left+r.width<=640);
                assert(r.top>=0 && r.top+r.height<=height);
                assert(r.results_height>=32 && r.results_height<=94);
                assert(r.favourites_y>=88+r.results_height+6);
                assert(r.buttons_y>=r.favourites_y+18+4);
                assert(r.status_y>=r.buttons_y+18+6);
                assert(r.status_y+18<=r.height-10);
            }
    assert(gadtools_radio_layout(&r,640,256,600,250));
    assert(r.height==256 && r.results_height==88 && r.top==0 && r.left==92);
    assert(gadtools_radio_layout(&r,640,512,40,30));
    assert(r.height==262 && r.results_height==94 && r.left==70 && r.top==60);
    puts("GadTools radio layout tests passed (PAL, bounds and compact layout)");
    return 0;
}
