#include "core/config_schema.h"

namespace htmlppt {

std::string_view ConfigSchema() {
  return R"json({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "htmlppt watchdog config",
  "type": "object",
  "required": ["version"],
  "properties": {
    "version": { "const": 1 },
    "activeProfile": { "type": "string", "minLength": 1 },
    "deck": { "type": "string" },
    "stateDir": { "type": "string" },
    "stage": { "$ref": "#/$defs/stage" },
    "defaults": { "$ref": "#/$defs/defaults" },
    "targets": { "type": "array", "items": { "$ref": "#/$defs/target" } },
    "profiles": {
      "type": "object",
      "additionalProperties": { "$ref": "#/$defs/profile" }
    },
    "legacy": { "$ref": "#/$defs/legacy" }
  },
  "$defs": {
    "cover": {
      "type": "object",
      "properties": {
        "fadeMs": { "type": "integer", "minimum": 1 },
        "holdMs": { "type": "integer", "minimum": 0 },
        "maxHoldMs": { "type": "integer", "minimum": 0 }
      },
      "additionalProperties": false
    },
    "edge": {
      "type": "object",
      "properties": {
        "maxLiveMs": { "type": "integer", "minimum": 1 },
        "prewarmProfile": { "type": "boolean" },
        "args": { "type": "array", "items": { "type": "string" } }
      },
      "additionalProperties": false
    },
    "display": {
      "type": "object",
      "properties": { "monitor": { "type": "string", "minLength": 1 } },
      "additionalProperties": false
    },
    "focus": {
      "type": "object",
      "properties": {
        "strategy": { "type": "string", "minLength": 1 },
        "require": { "type": "boolean" }
      },
      "additionalProperties": false
    },
    "assets": {
      "type": "object",
      "properties": {
        "mode": { "enum": ["file", "tree", "direct"] },
        "root": { "type": "string" },
        "include": { "type": "array", "items": { "type": "string" } },
        "exclude": { "type": "array", "items": { "type": "string" } },
        "maxBytes": { "type": "integer", "minimum": 0 }
      },
      "additionalProperties": false
    },
    "match": {
      "type": "object",
      "properties": {
        "slideTitle": { "type": "string", "minLength": 1 },
        "slideName": { "type": "string", "minLength": 1 },
        "mode": { "enum": ["exact", "prefix", "regex"] }
      },
      "additionalProperties": false,
      "oneOf": [
        { "required": ["slideTitle"] },
        { "required": ["slideName"] }
      ]
    },
    "target": {
      "type": "object",
      "properties": {
        "slide": { "type": "integer", "minimum": 1 },
        "match": { "$ref": "#/$defs/match" },
        "html": { "type": "string", "minLength": 1 },
        "label": { "type": "string" },
        "edgeProfile": { "type": "string" },
        "stageName": { "type": "string", "pattern": "^[ -~]*$" },
        "assets": { "$ref": "#/$defs/assets" },
        "htmlQuery": { "type": "string" },
        "trigger": { "enum": ["leave-forward", "arrive"] },
        "cover": { "$ref": "#/$defs/cover" },
        "edge": { "$ref": "#/$defs/edge" },
        "display": { "$ref": "#/$defs/display" },
        "focus": { "$ref": "#/$defs/focus" },
        "onFailure": { "enum": ["self-heal", "strict", "safe", "legacy"] },
        "maxConsecutiveTriggerFailures": { "type": "integer", "minimum": 0 },
        "enabled": { "type": "boolean" },
        "tags": { "type": "object", "additionalProperties": { "type": "string" } }
      },
      "required": ["html"],
      "oneOf": [
        { "required": ["slide"] },
        { "required": ["match"] }
      ],
      "additionalProperties": false
    },
    "defaults": {
      "type": "object",
      "properties": {
        "trigger": { "enum": ["leave-forward", "arrive"] },
        "cover": { "$ref": "#/$defs/cover" },
        "edge": { "$ref": "#/$defs/edge" },
        "display": { "$ref": "#/$defs/display" },
        "focus": { "$ref": "#/$defs/focus" },
        "scheduling": { "enum": ["adaptive", "aggressive", "balanced", "safe"] },
        "onFailure": { "enum": ["self-heal", "strict", "safe", "legacy"] },
        "maxConsecutiveTriggerFailures": { "type": "integer", "minimum": 0 }
      },
      "additionalProperties": false
    },
    "stage": {
      "type": "object",
      "properties": {
        "mode": { "enum": ["file", "tree", "direct"] },
        "root": { "type": "string" }
      },
      "additionalProperties": false
    },
    "profile": {
      "type": "object",
      "properties": {
        "deck": { "type": "string" },
        "stateDir": { "type": "string" },
        "stage": { "$ref": "#/$defs/stage" },
        "targets": { "type": "array", "items": { "$ref": "#/$defs/target" } }
      },
      "additionalProperties": false
    },
    "legacy": {
      "type": "object",
      "properties": {
        "deckName": { "type": "string" },
        "targetSlides": { "type": "array", "items": { "type": "integer", "minimum": 1 } },
        "srcNames": { "type": "array", "items": { "type": "string" } },
        "targetHtmls": { "type": "array", "items": { "type": "string" } },
        "targetProfiles": { "type": "array", "items": { "type": "string" } },
        "targetLabels": { "type": "array", "items": { "type": "string" } }
      },
      "additionalProperties": false
    }
  }
})json";
}

} // namespace htmlppt
