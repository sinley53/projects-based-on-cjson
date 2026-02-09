#include <stdio.h>
#include <cjson/cJSON.h>
#include <stdlib.h>

int main() {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "cJSON Test");
    cJSON_AddNumberToObject(root, "version", 1.7);

    char *json_str = cJSON_Print(root);
    printf("Generated JSON:\n%s\n", json_str);

    cJSON_Delete(root);
    free(json_str);
    return 0;
}