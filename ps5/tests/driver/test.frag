#version 450
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 0, binding = 1) uniform sampler2D target; // the picture being drawn on
layout(set = 0, binding = 2) uniform Constants { vec4 tint; } constants;
void main()
{
    out_color = texture(tex, in_uv) * constants.tint + texelFetch(target, ivec2(gl_FragCoord.xy), 0) * 0.5;
}
