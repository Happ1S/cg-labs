#version 450

// === Входные данные (из Vertex Shader) ===
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragWorldPos;
layout(location = 2) in vec2 fragUV;                 // Приходят уже масштабированные UV
layout(location = 3) in vec3 fragColor;
layout(location = 7) in vec4 fragLightSpacePos;      // Позиция фрагмента в clip-space света

layout(location = 0) out vec4 outColor;

// === Глобальные данные сцены ===
layout(std140, binding = 0) uniform SceneUBO {
    mat4 projection;
    mat4 view;
    vec3 viewPos;
    mat4 lightSpaceMatrix;
} scene;

// Направленный свет (Луна/Солнце)
layout(binding = 2) uniform DirectionalLightUBO {
    vec4 direction; // xyz = dir, w не используется
    vec4 color;     // rgb = цвет, a = интенсивность
} dirLight;

// Структуры точечных источников и прожекторов
struct PointLight {
    vec3 position;
    float constant;
    vec4 color;
    float linear;
    float quadratic;
    vec2 padding;
};

struct SpotLight {
    vec3 position;
    float constant;
    vec3 direction;
    float linear;
    vec4 color;
    float quadratic;
    float cutOff;
    float outerCutOff;
    float padding;
};

layout(std140, binding = 3) readonly buffer PointLightSSBO {
    PointLight pointLights[];
};

layout(std140, binding = 4) readonly buffer SpotLightSSBO {
    SpotLight spotLights[];
};

// === ТЕКСТУРА ===
layout(binding = 5) uniform sampler2D texSampler;

// === SHADOW MAP ===
layout(binding = 6) uniform sampler2DShadow shadowMap;
layout(binding = 7) uniform sampler2D debugShadowMapRaw; // Для отладки сырой глубины

// === ПАРАМЕТРЫ МАТЕРИАЛА (Push Constants) ===
layout(push_constant) uniform Push {
    mat4 model;
    mat4 normalMatrix;
    vec4 color;
    float metallic;
    float roughness;
    vec2 uvScale;
    float useTexture;
    float debugShadowMap; // Новый флаг для отладки shadow map
} push;

// Shadow factor: 0.0 = в тени, 1.0 = на свету
float computeShadow(vec4 lightSpacePos, vec3 normal, vec3 lightDir)
{
    // clip -> ndc
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;

    // ВАЖНО для Vulkan: remap [-1,1] -> [0,1] делаем только для X/Y.
    proj.xy = proj.xy * 0.5 + 0.5;

    // Если вне shadow map — считаем что освещено
    if (proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0 ||
        proj.z < 0.0 || proj.z > 1.0)
    {
        return 1.0;
    }

    // Bias: чуть больше при малых углах (уменьшает acne)
    float ndotl = max(dot(normal, lightDir), 0.0);
    float bias = max(0.002 * (1.0 - ndotl), 0.0005);

    // sampler2DShadow: texture() возвращает результат depth compare (обычно 0..1)
    float shadow = texture(shadowMap, vec3(proj.xy, proj.z - bias));

    // Если хотите прям совсем "рубленые" тени, можно раскомментировать:
    // shadow = step(0.999, shadow);

    return shadow;
}

void main()
{
    if (push.debugShadowMap > 0.5) {
        vec3 proj = fragLightSpacePos.xyz / fragLightSpacePos.w;
        proj.xy = proj.xy * 0.5 + 0.5;
        // Диагностика:
        // - Пурпурный = координаты вне карты (мы вообще не должны семплировать)
        // - Иначе показываем 1-depth, чтобы "пустая карта" (depth=1) выглядела ЧЁРНОЙ, а близкие поверхности — светлее.
        if (proj.x < 0.0 || proj.x > 1.0 ||
            proj.y < 0.0 || proj.y > 1.0 ||
            proj.z < 0.0 || proj.z > 1.0)
        {
            outColor = vec4(1.0, 0.0, 1.0, 1.0); // вне shadow map
            return;
        }
        
        // Визуализируем значение глубины из shadow map
        float depthValue = texture(debugShadowMapRaw, proj.xy).r; // Читаем сырое значение глубины
        outColor = vec4(vec3(1.0 - depthValue), 1.0);
        return;
    }

    // Луна/эмиссивный объект: не освещаем directional/point/spot, а просто рисуем текстуру/цвет.
    // Используем флаг через push.metallic < 0, чтобы не менять layout push-констант.
    if (push.metallic < -0.5) {
        // Небо: equirectangular панорама (sky.jpg 5000x3000 и т.п.), растягиваем на весь купол.
        // Используем push.useTexture > 2.5 как флаг "sky".
        if (push.useTexture > 2.5) {
            const float PI = 3.14159265359;
            vec3 dir = normalize(fragWorldPos - scene.viewPos);

            float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
            float v = 0.5 - asin(clamp(dir.y, -1.0, 1.0)) / PI;
            v = clamp(v, 0.001, 0.999);

            // Отзеркаливание для минимизации шва:
            // используем mirrored repeat по U. В таком режиме граница тайла стыкует одинаковые края текстуры,
            // поэтому даже неидеально бесшовная панорама даёт гораздо меньше заметный шов.
            float uScale = max(push.uvScale.x, 1.0);
            float uScaled = u * uScale;
            float uWrap = 1.0 - abs(fract(uScaled * 0.5) * 2.0 - 1.0); // mirrored repeat -> [0..1]

            vec3 sky = texture(texSampler, vec2(uWrap, v)).rgb;

            outColor = vec4(sky * push.color.rgb, 1.0);
            return;
        }

        // "Круглая" текстура луны: проецируем текстуру как диск на сферу, ориентируя диск на камеру.
        // Это подходит для PNG/JPG с круглым изображением луны (и прозрачным/тёмным фоном).
        vec3 center = push.model[3].xyz;
        vec3 axis = normalize(scene.viewPos - center); // куда "смотрит" луна (к камере)

        vec3 upRef = vec3(0.0, 1.0, 0.0);
        if (abs(dot(axis, upRef)) > 0.99) {
            upRef = vec3(0.0, 0.0, 1.0);
        }
        vec3 right = normalize(cross(upRef, axis));
        vec3 up = normalize(cross(axis, right));

        vec3 dir = normalize(fragWorldPos - center); // направление от центра сферы

        // На "задней" стороне диска — рисуем чёрным (луна не светится назад).
        float z = dot(dir, axis);
        if (z <= 0.0) {
            outColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }

        float x = dot(dir, right);
        float y = dot(dir, up);

        float r = length(vec2(x, y));
        // мягкий край диска (anti-alias)
        float mask = 1.0 - smoothstep(0.995, 1.0, r);
        if (mask <= 0.0) {
            outColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }

        vec2 uv = vec2(x, y) * 0.5 + 0.5;
        vec4 t = texture(texSampler, uv);

        // Если у текстуры есть alpha (PNG) — учитываем её.
        float a = max(t.a, 0.0);
        float m = mask * a;

        vec3 albedo = (push.useTexture > 0.5) ? (t.rgb * push.color.rgb) : push.color.rgb;
        outColor = vec4(albedo * m, 1.0);
        return;
    }
    // 1) Albedo из текстуры/цвета
    vec2 uv = fragUV;
    vec3 texColor = texture(texSampler, uv).rgb;

    vec3 albedo = (push.useTexture > 0.5)
        ? (texColor * push.color.rgb)
        : push.color.rgb;

    vec3 N = normalize(fragNormal);
    vec3 V = normalize(scene.viewPos - fragWorldPos);

    // 2) Подготовка "псевдо-PBR" параметров (как у вас)
    // Для "очень блестящих" материалов используем clearcoat-слой.
    // Флаг берём из push.useTexture:
    // - 0.0/1.0: обычное поведение
    // - 2.0: текстура + clearcoat (например, лак на кузове)
    float clearcoat = clamp(push.useTexture - 1.0, 0.0, 1.0);
    float metallic = clamp(push.metallic, 0.0, 1.0);
    float roughness = clamp(push.roughness, 0.0, 1.0);
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    vec3 ambient = albedo * 0.001;

    // 3) Направленный свет + тень
    vec3 Ld = normalize(-dirLight.direction.xyz);

    float diff = max(dot(N, Ld), 0.0);

    vec3 diffuse = albedo * dirLight.color.rgb * diff * dirLight.color.a * (1.0 - metallic);

    vec3 R = reflect(-Ld, N);
    float shininess = max((1.0 - roughness) * 256.0, 1.0);

    float spec = pow(max(dot(V, R), 0.0), shininess);
    vec3 specular = F0 * spec * dirLight.color.rgb * dirLight.color.a * 2.5;

    // Clearcoat: очень узкий и яркий блик (например, лак на кузове).
    float coatShininess = 2048.0;
    float coatSpec = pow(max(dot(V, R), 0.0), coatShininess);
    vec3 coatColor = vec3(0.04); // диэлектрический слой лака
    vec3 clearcoatSpecular = coatColor * coatSpec * dirLight.color.rgb * dirLight.color.a * (6.0 * clearcoat);

    float shadow = computeShadow(fragLightSpacePos, N, Ld);

    // Если хотите "очень темные" тени — как у вас:
    shadow = mix(0.02, 1.0, shadow);

    vec3 totalLight = ambient + (diffuse + specular + clearcoatSpecular) * shadow;

    // 4) Point lights (без теней)
    for (int i = 0; i < 8 && i < pointLights.length(); i++)
    {
        vec3 lightPos = pointLights[i].position;
        vec3 lightVec = lightPos - fragWorldPos;

        float dist = length(lightVec);
        vec3 L = lightVec / max(dist, 1e-6);

        float attenuation = 1.0 / (pointLights[i].constant +
                                   pointLights[i].linear * dist +
                                   pointLights[i].quadratic * dist * dist);

        float diffP = max(dot(N, L), 0.0);
        vec3 diffuseP = albedo * pointLights[i].color.rgb * diffP * attenuation * pointLights[i].color.a * (1.0 - metallic);

        vec3 RP = reflect(-L, N);
        float specP = pow(max(dot(V, RP), 0.0), shininess);
        vec3 specularP = F0 * specP * pointLights[i].color.rgb * attenuation * pointLights[i].color.a;

        float coatSpecP = pow(max(dot(V, RP), 0.0), coatShininess);
        vec3 clearcoatP = coatColor * coatSpecP * pointLights[i].color.rgb * attenuation * pointLights[i].color.a * (6.0 * clearcoat);

        totalLight += diffuseP + specularP + clearcoatP;
    }

    // 5) Spot lights (без теней)
    for (int i = 0; i < 2 && i < spotLights.length(); i++)
    {
        vec3 toLight = spotLights[i].position - fragWorldPos;
        float dist = length(toLight);
        vec3 L = toLight / max(dist, 1e-6);

        vec3 S = normalize(spotLights[i].direction);

        float theta = dot(-L, S);
        float epsilon = spotLights[i].cutOff - spotLights[i].outerCutOff;
        float intensity = clamp((theta - spotLights[i].outerCutOff) / max(epsilon, 1e-6), 0.0, 1.0);

        if (intensity > 0.0)
        {
            float attenuation = 1.0 / (spotLights[i].constant +
                                       spotLights[i].linear * dist +
                                       spotLights[i].quadratic * dist * dist);

            float diffS = max(dot(N, L), 0.0);
            vec3 diffuseS = albedo * spotLights[i].color.rgb * diffS * attenuation * spotLights[i].color.a * intensity * (1.0 - metallic);

            vec3 RS = reflect(-L, N);
            float specS = pow(max(dot(V, RS), 0.0), shininess);
            vec3 specularS = F0 * specS * spotLights[i].color.rgb * attenuation * spotLights[i].color.a * intensity;

            float coatSpecS = pow(max(dot(V, RS), 0.0), coatShininess);
            vec3 clearcoatS = coatColor * coatSpecS * spotLights[i].color.rgb * attenuation * spotLights[i].color.a * intensity * (6.0 * clearcoat);

            totalLight += diffuseS + specularS + clearcoatS;
        }
    }

    outColor = vec4(totalLight, 1.0);
}
