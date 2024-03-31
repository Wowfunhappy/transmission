/******************************************************************************
 * Copyright (c) 2011-2012 Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#import "NSImageAdditions.h"

@implementation NSImage (NSImageAdditions)

- (NSImage *) imageWithColor: (NSColor *) color
{
    NSImage * coloredImage = [self copy];

    [coloredImage lockFocus];

    [color set];

    const NSSize size = [coloredImage size];
    NSRectFillUsingOperation(NSMakeRect(0.0, 0.0, size.width, size.height), NSCompositeSourceAtop);

    [coloredImage unlockFocus];

    return coloredImage;
}

- (NSImage *) templateImageNamed: (NSString *) name {
    NSImage *img = [NSImage imageNamed: name];
    [img wantsTemplate: YES];
    return img;
}

@end
