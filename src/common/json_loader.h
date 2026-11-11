//
//  json_loader.h
//  c-ray
//
//  Created by Valtteri Koskivuori on 02/04/2019.
//  Copyright © 2019-2023 Valtteri Koskivuori. All rights reserved.
//

#pragma once

#include <c-ray/c-ray.h>

struct cr_renderer;
struct cJSON;

int parse_json(struct cr_renderer *r, struct cJSON *json);
