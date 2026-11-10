#version 330

// Second half of the shadow pass: takes the shadow term as the march produced
// it, blurs it, and averages it with what was there a moment ago.
//
// The march is a yes/no question asked of a noisy depth map, so its answer
// has ragged edges that move every frame. Neither is something a real shadow
// does. The blur gives the edge a penumbra; the average over time means a
// texel has to stay shadowed for a few frames before it reads as shadow.

in vec2 fragTexCoord;

uniform sampler2D texture0;   // this frame's shadow term: 1 lit, 0 shadowed
uniform sampler2D history;    // the filtered result of the previous frame
uniform vec2 texel;           // 1.0 / target size
uniform float blend;          // share of this frame in the result; 1 = no history

out vec4 finalColor;

void main()
{
    // 3x3 tent, with the taps pushed out to 1.5 texels so the bilinear filter
    // widens each one: about a five-texel blur for nine fetches.
    vec2 d = texel*1.5;
    float sum = texture(texture0, fragTexCoord).r*4.0;
    sum += (texture(texture0, fragTexCoord + vec2( d.x, 0.0)).r +
            texture(texture0, fragTexCoord + vec2(-d.x, 0.0)).r +
            texture(texture0, fragTexCoord + vec2(0.0,  d.y)).r +
            texture(texture0, fragTexCoord + vec2(0.0, -d.y)).r)*2.0;
    sum += texture(texture0, fragTexCoord + vec2( d.x,  d.y)).r +
           texture(texture0, fragTexCoord + vec2(-d.x,  d.y)).r +
           texture(texture0, fragTexCoord + vec2( d.x, -d.y)).r +
           texture(texture0, fragTexCoord + vec2(-d.x, -d.y)).r;
    float now = sum/16.0;

    // The same texel of the previous result. gl_FragCoord rather than
    // fragTexCoord: drawing one render texture into another flips it, and the
    // pixel's own position is the one thing that does not depend on that.
    float before = texture(history, gl_FragCoord.xy*texel).r;

    finalColor = vec4(vec3(mix(before, now, blend)), 1.0);
}
