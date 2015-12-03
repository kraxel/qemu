
#version 300 es
#extension GL_OES_EGL_image_external : require

uniform samplerExternalOES image;
in  mediump vec2 ex_tex_coord;
out mediump vec4 out_frag_color;

void main(void) {
     out_frag_color = texture2D(image, ex_tex_coord);
}
