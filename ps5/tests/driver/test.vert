#version 450
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec2 out_uv;
layout(push_constant) uniform Push { vec4 offset; } push;
void main()
{
    out_uv = in_uv;
    gl_Position = vec4(in_pos + push.offset.xy, 0.0, 1.0);
}
