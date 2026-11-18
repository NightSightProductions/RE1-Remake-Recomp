// usage: winids NAME: print "id title" for on-screen windows owned by processes called NAME (for screencapture -l)
#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    CFArrayRef a = CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID);
    for (CFIndex i = 0; i < CFArrayGetCount(a); i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(a, i);
        char owner[256] = "", name[256] = ""; int id = 0;
        CFStringRef o = CFDictionaryGetValue(d, kCGWindowOwnerName), n = CFDictionaryGetValue(d, kCGWindowName);
        if (o) CFStringGetCString(o, owner, sizeof owner, kCFStringEncodingUTF8);
        if (n) CFStringGetCString(n, name, sizeof name, kCFStringEncodingUTF8);
        CFNumberGetValue(CFDictionaryGetValue(d, kCGWindowNumber), kCFNumberIntType, &id);
        if (argc > 1 && !strcmp(owner, argv[1]) && name[0]) printf("%d %s\n", id, name);
    }
    return 0;
}
