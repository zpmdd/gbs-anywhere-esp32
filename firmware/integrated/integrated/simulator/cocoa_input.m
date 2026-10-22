#import <Cocoa/Cocoa.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <stdbool.h>

static id monitor;

// SDL2 can lose AppKit drag events and reuse a stale cursor position on release.
// Read this window's content coordinates directly; leave window chrome to AppKit.
bool gbs_cocoa_input(SDL_Window *window, void (*pointer)(int, int, bool))
{
    SDL_SysWMinfo info; SDL_VERSION(&info.version);
    if(!SDL_GetWindowWMInfo(window,&info)) return false;
    NSWindow *native=info.info.cocoa.window;
    __block bool held=false;
    monitor=[NSEvent addLocalMonitorForEventsMatchingMask:
        NSEventMaskLeftMouseDown|NSEventMaskLeftMouseUp|NSEventMaskLeftMouseDragged
        handler:^NSEvent *(NSEvent *event) {
            if(event.window!=native) return event;
            NSView *view=native.contentView;
            NSPoint point=[view convertPoint:event.locationInWindow fromView:nil];
            if(event.type==NSEventTypeLeftMouseDown) {
                if(!NSPointInRect(point,view.bounds)) return event;
                held=true;
            } else if(!held) return event;
            bool down=event.type!=NSEventTypeLeftMouseUp;
            pointer((int)point.x,(int)(view.bounds.size.height-point.y),down);
            held=down;
            return nil;
        }];
    return monitor!=nil;
}

void gbs_cocoa_input_stop(void)
{
    if(monitor) [NSEvent removeMonitor:monitor];
    monitor=nil;
}
