#pragma once

namespace wp::video {

bool make_gl_current();
void* metal_view();
void park_while_background();
bool background_wanted();
int vulkan_window();
int display_rate(int wanted, int& highest);

}
