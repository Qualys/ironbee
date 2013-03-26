/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

#include "ironbee_config_auto.h"

#include "engine_private.h"

#include <ironbee/action.h>
#include <ironbee/cfgmap.h>
#include <ironbee/config.h>
#include <ironbee/core.h>
#include <ironbee/engine.h>
#include <ironbee/list.h>
#include <ironbee/lock.h>
#include <ironbee/module.h>
#include <ironbee/mpool.h>
#include <ironbee/operator.h>
#include <ironbee/path.h>
#include <ironbee/provider.h>
#include <ironbee/rule_engine.h>
#include <ironbee/string.h>
#include <ironbee/util.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <strings.h>

#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Define the module name as well as a string version of it. */
#define MODULE_NAME        rules
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

/* Declare the public module symbol. */
IB_MODULE_DECLARE();

/**
 * Parse rule's operator.
 *
 * Parses the rule's operator and operand strings (@a operator and @a
 * operand), and stores the results in the rule object @a rule.
 *
 * @param cp IronBee configuration parser
 * @param rule Rule object to update
 * @param operator Operator string
 * @param operand Operand string
 *
 * @returns Status code
 */
static ib_status_t parse_operator(ib_cfgparser_t *cp,
                                  ib_rule_t *rule,
                                  const char *operator,
                                  const char *operand)
{
    assert(cp != NULL);
    assert(rule != NULL);
    assert(operator != NULL);

    ib_status_t rc = IB_OK;
    const char *opname = NULL;
    const char *cptr = operator;
    ib_flags_t flags = IB_OPINST_FLAG_NONE;
    ib_operator_inst_t *opinst;

    /* Leading '!' (invert flag)? */
    if (*cptr == '!') {
        flags |= IB_OPINST_FLAG_INVERT;
        ++cptr;
    }

    /* Better be an '@' next... */
    if ( (*cptr != '@') || (isalpha(*(cptr+1)) == 0) ) {
        ib_cfg_log_error(cp, "Invalid rule syntax \"%s\"", operator);
        return IB_EINVAL;
    }
    opname = cptr + 1;

    /* Create the operator instance */
    rc = ib_operator_inst_create(cp->ib,
                                 cp->cur_ctx,
                                 rule,
                                 ib_rule_required_op_flags(rule),
                                 opname,
                                 operand,
                                 flags,
                                 &opinst);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Failed to create operator instance "
                         "operator=\"%s\" operand=\"%s\": %s",
                         opname,
                         operand == NULL ? "" : operand,
                         ib_status_to_string(rc));
        return rc;
    }

    /* Set the operator */
    rc = ib_rule_set_operator(cp->ib, rule, opinst);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Failed to set operator for rule: %s",
                         ib_status_to_string(rc));
        return rc;
    }
    ib_cfg_log_debug3(cp,
                      "Rule: operator=\"%s\" operand=\"%s\" "
                      "flags=0x%04x",
                      operator,
                      (operand == NULL) ? "" : operand,
                      flags);

    return rc;
}

/**
 * Rewrite the target string if required
 *
 * Parses the rule's target field list string @a target_str, looking for
 * the '&' tokens at the start of it.
 *
 * @param[in] cp IronBee configuration parser
 * @param[in] target_str Target field name.
 * @param[out] rewritten Rewritten string.
 * @param[out] rewrites Number of rewrites found in @a target_str.
 *
 * @returns Status code
 */
#define MAX_TFN_TOKENS 10
static ib_status_t rewrite_target_tokens(ib_cfgparser_t *cp,
                                         const char *target_str,
                                         const char **rewritten,
                                         int *rewrites)
{
    char const *ops[MAX_TFN_TOKENS];
    const char *cur = target_str;
    char *new;
    int count = 0;
    int n;
    int target_len = strlen(target_str) + 1;

    /**
     * Loop 'til we reach max count
     */
    while ( (*cur != '\0') && (count < MAX_TFN_TOKENS) ) {
        if (*cur == '&') {
            ops[count] = ".count()";
        }
        else {
            break;
        }

        /* Update the required length */
        target_len += strlen(ops[count]) - 1;
        ++count;
        ++cur;
    }

    /* No rewrites?  Done */
    *rewrites = count;
    if (count == 0) {
        *rewritten = target_str;
        return IB_OK;
    }

    /**
     * Allocate & build the new string
     */
    new = ib_mpool_alloc(cp->mp, target_len);
    if (new == NULL) {
        ib_cfg_log_error(cp,
                         "Failed to duplicate target field string \"%s\"",
                         target_str);
        return IB_EALLOC;
    }

    /* Add the functions in reverse order */
    strcpy(new, target_str+count);
    for (n = count-1;  n >= 0;  --n) {
        strcat(new, ops[n] );
    }

    /* Log our rewrite */
    ib_cfg_log_debug3(cp, "Rewrote \"%s\" -> \"%s\"", target_str, new);

    /* Done */
    *rewritten = new;
    return IB_OK;
}

/**
 * Parse the transformations from a target string
 *
 * @param[in] cp Configuration parser
 * @param[in] str Target field string to parse
 * @param[out] target Target name
 * @param[out] tfns List of transformation names
 *
 * @returns Status code
 */
static ib_status_t parse_target_string(ib_cfgparser_t *cp,
                                       const char *str,
                                       const char **target,
                                       ib_list_t **tfns)
{
    ib_status_t  rc;
    char        *cur;                /* Current position */
    char        *dup_str;            /* Duplicate string */

    assert(cp != NULL);
    assert(str != NULL);
    assert(target != NULL);

    /* Start with a known state */
    *target = NULL;
    *tfns = NULL;

    /* No parens?  Just store the target string as the field name & return. */
    if (strstr(str, "()") == NULL) {
        *target = str;
        return IB_OK;
    }

    /* Make a duplicate of the target string to work on */
    dup_str = ib_mpool_strdup(ib_rule_mpool(cp->ib), str);
    if (dup_str == NULL) {
        ib_cfg_log_error(cp, "Error duplicating target string \"%s\"", str);
        return IB_EALLOC;
    }

    /* Walk through the string */
    cur = dup_str;
    while (cur != NULL) {
        char  *separator;       /* Current separator */
        char  *parens = NULL;   /* Paren pair '()' */
        char  *pdot = NULL;     /* Paren pair + dot '().' */
        char  *tfn = NULL;      /* Transformation name */

        /* First time through the loop? */
        if (cur == dup_str) {
            separator = strchr(cur, '.');
            if (separator == NULL) {
                break;
            }
            *separator = '\0';
            tfn = separator + 1;
        }
        else {
            separator = cur;
            tfn = separator;
        }

        /* Find the next separator and paren set */
        parens = strstr(separator+1, "()");
        pdot = strstr(separator+1, "().");

        /* Parens + dot: intermediate transformation */
        if (pdot != NULL) {
            *pdot = '\0';
            *(pdot+2) = '\0';
            cur = pdot + 3;
        }
        /* Parens but no dot: last transformation */
        else if (parens != NULL) {
            *parens = '\0';
            cur = NULL;
        }
        /* Finally, no parens: done */
        else {
            cur = NULL;
            tfn = NULL;
        }

        /* Skip to top of loop if there's no operator */
        if (tfn == NULL) {
            continue;
        }

        /* Create the transformation list if required. */
        if (*tfns == NULL) {
            rc = ib_list_create(tfns, ib_rule_mpool(cp->ib));
            if (rc != IB_OK) {
                ib_cfg_log_error(cp,
                                 "Error creating transformation list: %s",
                                 ib_status_to_string(rc));
                return rc;
            }
        }

        /* Add the name to the list */
        rc = ib_list_push(*tfns, tfn);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp,
                             "Error adding transformation \"%s\" to list: %s",
                             tfn, ib_status_to_string(rc));
            return rc;
        }
    }

    /**
     * The field name is the start of the duplicate string, even after
     * it's been chopped up into pieces.
     */
    *target = dup_str;
    ib_cfg_log_debug3(cp, "Final target field name is \"%s\"", *target);

    return IB_OK;
}

/**
 * Parse a rule's target string.
 *
 * Parses the rule's target field list string @a target_str, and stores the
 * results in the rule object @a rule.
 *
 * @param[in] cp IronBee configuration parser
 * @param[in,out] rule Rule to operate on
 * @param[in] target_str Target string to parse
 *
 * @returns
 *  - IB_OK if there is one or more targets.
 *  - IB_EINVAL if not targets are found, including if the string is empty.
 *  - IB_EALLOC if a memory allocation fails.
 */
static ib_status_t parse_target(ib_cfgparser_t *cp,
                                ib_rule_t *rule,
                                const char *target_str)
{
    ib_status_t rc;
    const char *rewritten_target_str = NULL;
    const char *final_target_str; /* Holder for the final target name. */
    ib_list_t *tfns;              /* Transformations to perform. */
    ib_rule_target_t *ib_rule_target;
    int not_found = 0;
    int rewrites;

    /* First, rewrite cur into rewritten_target_str. */
    rc = rewrite_target_tokens(cp, target_str,
                               &rewritten_target_str, &rewrites);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error rewriting target \"%s\"", target_str);
        return rc;
    }

    /* Parse the rewritten string into the final_target_str. */
    rc = parse_target_string(cp,
                             rewritten_target_str,
                             &final_target_str,
                             &tfns);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error parsing target string \"%s\": %s",
                         target_str, ib_status_to_string(rc));
        return rc;
    }

    /* Create the target object */
    rc = ib_rule_create_target(cp->ib,
                               target_str,
                               final_target_str,
                               tfns,
                               &ib_rule_target,
                               &not_found);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Error creating rule target \"%s\": %s",
                         final_target_str, ib_status_to_string(rc));
        return rc;
    }
    else if (not_found != 0) {
        ib_cfg_log_error(cp,
            "Rule target \"%s\": %d transformations not found",
            final_target_str, not_found
        );
        return IB_EINVAL;
    }

    /* Add the target to the rule */
    rc = ib_rule_add_target(cp->ib, rule, ib_rule_target);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to add rule target \"%s\"", target_str);
        return rc;
    }
    ib_cfg_log_debug3(cp, "Added rule target \"%s\" to rule", target_str);

    return IB_OK;
}

/**
 * Attempt to register a string as an action.
 *
 * Treats the rule's modifier string @a name as a action, and registers
 * the appropriate action with @a rule.
 *
 * @param[in] cp IronBee configuration parser
 * @param[in,out] rule Rule to operate on
 * @param[in] name Action name
 * @param[in] params Parameters string
 *
 * @returns Status code
 */
static ib_status_t register_action_modifier(ib_cfgparser_t *cp,
                                            ib_rule_t *rule,
                                            const char *name,
                                            const char *params)
{
    ib_status_t        rc = IB_OK;
    ib_action_inst_t  *action;
    ib_rule_action_t   atype = RULE_ACTION_TRUE;
    if (*name == '!') {
        ++name;
        atype = RULE_ACTION_FALSE;
    }

    /* Create a new action instance */
    rc = ib_action_inst_create(cp->ib,
                               cp->cur_ctx,
                               name,
                               params,
                               IB_ACTINST_FLAG_NONE,
                               &action);
    if (rc == IB_ENOENT) {
        ib_cfg_log_notice(cp, "Ignoring unknown modifier \"%s\"", name);
        return IB_OK;
    }
    else if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Failed to create action instance \"%s\": %s",
                         name, ib_status_to_string(rc));
        return rc;
    }

    /* Add the action to the rule */
    rc = ib_rule_add_action(cp->ib, rule, action, atype);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Failed to add action \"%s\" to rule \"%s\": %s",
                         name, ib_rule_id(rule), ib_status_to_string(rc));
        return rc;
    }

    return IB_OK;
}

/**
 * Check that a rule has all the proper modifiers.
 *
 * @param[in] cp The configuration parser
 * @param[in] rule The rule to check
 *
 * @returns IB_EINVAL.
 */
static ib_status_t check_rule_modifiers(ib_cfgparser_t *cp,
                                        ib_rule_t *rule)
{
    bool child = ib_flags_all(rule->flags, IB_RULE_FLAG_CHCHILD);

    if ( (! child) && (ib_rule_id(rule) == NULL) )
    {
        ib_cfg_log_error(cp, "No rule id specified (flags=0x%04x)",
                         rule->flags);
        return IB_EINVAL;
    }

    if ( (! child) &&
         ((rule->meta.phase == PHASE_INVALID) ||
          (rule->meta.phase == PHASE_NONE)) )
    {
        ib_cfg_log_error(cp, "Phase invalid or not specified.");
        return IB_EINVAL;
    }

    return IB_OK;
}
/**
 * Parse a rule's modifier string.
 *
 * Parses the rule's modifier string @a modifier_str, and stores the results
 * in the rule object @a rule.
 *
 * @param[in] cp IronBee configuration parser
 * @param[in,out] rule Rule to operate on
 * @param[in] modifier_str Modifier string
 *
 * @returns Status code
 */
static ib_status_t parse_modifier(ib_cfgparser_t *cp,
                                  ib_rule_t *rule,
                                  const char *modifier_str)
{
    ib_status_t rc = IB_OK;
    const char *name;
    char *colon;
    char *copy;
    const char *value = NULL;

    assert(cp != NULL);
    assert(rule != NULL);
    assert(modifier_str != NULL);

    /* Copy the string */
    copy = ib_mpool_strdup(ib_rule_mpool(cp->ib), modifier_str);
    if (copy == NULL) {
        ib_cfg_log_error(cp,
                         "Failed to copy rule modifier \"%s\"", modifier_str);
        return IB_EALLOC;
    }

    /* Modifier name */
    name = copy;
    colon = strchr(copy, ':');
    if ( (colon != NULL) && ( *(colon+1) != '\0' ) ) {
        *colon = '\0';
        value = colon + 1;
        while( isspace(*value) ) {
            ++value;
        }
        if (*value == '\0') {
            value = NULL;
        }
    }

    /* ID modifier */
    if (strcasecmp(name, "id") == 0) {
        if (value == NULL) {
            ib_cfg_log_error(cp, "Modifier ID with no value");
            return IB_EINVAL;
        }
        rc = ib_rule_set_id(cp->ib, rule, value);
        return rc;
    }

    /* Message modifier */
    if (strcasecmp(name, "msg") == 0) {
        bool expand = false;
        rule->meta.msg = value;
        rc = ib_data_expand_test_str(value, &expand);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "Expansion test failed: %d", rc);
            return rc;
        }
        if (expand) {
            rule->meta.flags |= IB_RULEMD_FLAG_EXPAND_MSG;
        }
        return IB_OK;
    }

    /* LogData modifier */
    if (strcasecmp(name, "logdata") == 0) {
        bool expand = false;
        rule->meta.data = value;
        rc = ib_data_expand_test_str(value, &expand);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "Expansion test failed: %d", rc);
            return rc;
        }
        if (expand) {
            rule->meta.flags |= IB_RULEMD_FLAG_EXPAND_DATA;
        }
        return IB_OK;
    }

    /* Tag modifier */
    if (strcasecmp(name, "tag") == 0) {
        rc = ib_list_push(rule->meta.tags, (void *)value);
        return rc;
    }

    /* Severity modifier */
    if (strcasecmp(name, "severity") == 0) {
        int severity = value ? atoi(value) : 0;

        if (severity > UINT8_MAX) {
            ib_cfg_log_error(cp, "Invalid severity: %s", value);
            return IB_EINVAL;
        }
        rule->meta.severity = (uint8_t)severity;
        return IB_OK;
    }

    /* Confidence modifier */
    if (strcasecmp(name, "confidence") == 0) {
        int confidence = value ? atoi(value) : 0;

        if (confidence > UINT8_MAX) {
            ib_cfg_log_error(cp, "Invalid confidence: %s", value);
            return IB_EINVAL;
        }
        rule->meta.confidence = (uint8_t)confidence;
        return IB_OK;
    }

    /* Revision modifier */
    if (strcasecmp(name, "rev") == 0) {
        int rev = value ? atoi(value) : 0;

        if ( (rev < 0) || (rev > UINT16_MAX) ) {
            ib_cfg_log_error(cp, "Invalid revision: %s", value);
            return IB_EINVAL;
        }
        rule->meta.revision = (uint16_t)rev;
        return IB_OK;
    }

    /* Phase modifiers (Not valid for stream rules) */
    if (! ib_rule_is_stream(rule)) {
        ib_rule_phase_num_t phase = PHASE_NONE;
        if (strcasecmp(name, "phase") == 0) {
            if (value == NULL) {
                ib_cfg_log_error(cp, "Modifier PHASE with no value");
                return IB_EINVAL;
            }
            phase = ib_rule_lookup_phase(value, false);
            if (phase == PHASE_INVALID) {
                ib_cfg_log_error(cp, "Invalid phase: %s", value);
                return IB_EINVAL;
            }
        }
        else {
            ib_rule_phase_num_t tphase;
            tphase = ib_rule_lookup_phase(name, false);
            if (tphase != PHASE_INVALID) {
                phase = tphase;
            }
        }

        /* If we encountered a phase modifier, set it */
        if (phase != PHASE_NONE && phase != PHASE_INVALID) {
            rc = ib_rule_set_phase(cp->ib, rule, phase);
            if (rc != IB_OK) {
                ib_cfg_log_error(cp, "Error setting rule phase: %s",
                                 ib_status_to_string(rc));
                return rc;
            }
            return IB_OK;
        }

        /* Not a phase modifier, so don't return */
    }

    /* Chain modifier */
    if ( (ib_rule_allow_chain(rule)) &&
         (strcasecmp(name, "chain") == 0) )
    {
        rc = ib_rule_set_chain(cp->ib, rule);
        return rc;
    }

    /* Capture modifier */
    if (strcasecmp(name, "capture") == 0) {
        if (ib_flags_any(rule->opinst->op->flags, IB_OP_FLAG_CAPTURE)) {
            rule->flags |= IB_RULE_FLAG_CAPTURE;
            return IB_OK;
        }
        else {
            ib_cfg_log_error(cp, "Capture not supported by operator %s",
                             rule->opinst->op->name);
            return IB_EINVAL;
        }
    }

    /* Transformation modifiers */
    if (strcasecmp(name, "t") == 0) {
        if (! ib_rule_allow_tfns(rule)) {
            ib_cfg_log_error(cp,
                "Transformations not supported for this rule"
            );
            return IB_EINVAL;
        }

        if (value == NULL) {
            ib_cfg_log_error(cp, "Modifier transformation with no value");
            return IB_EINVAL;
        }
        rc = ib_rule_add_tfn(cp->ib, rule, value);
        if (rc == IB_ENOENT) {
            ib_cfg_log_error(cp, "Unknown transformation: \"%s\"", value);
            return rc;
        }
        else if (rc != IB_OK) {
            ib_cfg_log_error(cp, "Error adding transformation \"%s\": %s",
                             value, ib_status_to_string(rc));
            return IB_EINVAL;
        }
        return IB_OK;
    }

    /* Finally, try to match it to an action */
    rc = register_action_modifier(cp, rule, name, value);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error registering action \"%s\": %s",
                         name, ib_status_to_string(rc));
        return rc;
    }

    return rc;
}

/**
 * @brief Parse a RuleExt directive.
 * @details Register lua function. RuleExt lua:/path/to/rule.lua phase:REQUEST
 * @param[in,out] cp Configuration parser that contains the engine being
 *                configured.
 * @param[in] name The directive name.
 * @param[in] vars The list of variables passed to @a name.
 * @param[in] cbdata User data. Unused.
 */
static ib_status_t parse_ruleext_params(ib_cfgparser_t *cp,
                                        const char *name,
                                        const ib_list_t *vars,
                                        void *cbdata)
{
    ib_status_t rc;
    const ib_list_node_t *targets;
    const ib_list_node_t *mod;
    ib_rule_t *rule;
    const char *file_name;
    const char *colon;
    const char *tag;
    const char *location;

    /* Get the targets string */
    targets = ib_list_first_const(vars);

    file_name = (const char *)ib_list_node_data_const(targets);

    if ( file_name == NULL ) {
        ib_cfg_log_error(cp, "No targets for rule");
        return IB_EINVAL;
    }

    ib_cfg_log_debug3(cp, "Processing external rule: %s", file_name);

    /* Allocate a rule */
    rc = ib_rule_create(cp->ib, cp->cur_ctx,
                        cp->cur_file, cp->cur_lineno,
                        false, &rule);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to create rule: %s",
                         ib_status_to_string(rc));
        return rc;
    }
    ib_flags_set(rule->flags, (IB_RULE_FLAG_EXTERNAL | IB_RULE_FLAG_NO_TGT));

    /* Parse all of the modifiers */
    mod = targets;
    while( (mod = ib_list_node_next_const(mod)) != NULL) {
        ib_cfg_log_debug3(cp, "Parsing modifier %s", (const char *)mod->data);
        rc = parse_modifier(cp, rule, mod->data);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp,
                "Error parsing external rule modifier \"%s\": %s",
                (const char *)mod->data,
                ib_status_to_string(rc)
            );
            return rc;
        }
    }

    /* Check the rule modifiers. */
    rc = check_rule_modifiers(cp, rule);
    if (rc != IB_OK) {
        return rc;
    }

    /* Using the rule->meta and file_name, load and stage the ext rule. */
    ib_rule_driver_t *driver;
    colon = strchr(file_name, ':');
    if (colon == NULL) {
        ib_cfg_log_error(cp,
            "Could not parse external rule location: %s.  No colon found.",
            file_name
        );
        return IB_EINVAL;
    }
    tag = ib_mpool_memdup_to_str(cp->mp, file_name, colon - file_name);
    if (tag == NULL) {
        return IB_EALLOC;
    }
    location = ib_util_relative_file(cp->mp, cp->cur_file, colon + 1);
    if (location == NULL) {
        return IB_EALLOC;
    }
    rc = ib_rule_lookup_external_driver(cp->ib, tag, &driver);
    if (rc != IB_ENOENT) {
        rc = driver->function(cp, rule, tag, location, driver->cbdata);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp,
                             "Error in external rule driver for \"%s\": %s",
                             tag, ib_status_to_string(rc)
            );
            return rc;
        }
    }
    else {
        ib_cfg_log_error(cp, "No external rule driver for \"%s\"", tag);
        return IB_EINVAL;
    }

    /* Finally, register the rule */
    rc = ib_rule_register(cp->ib, cp->cur_ctx, rule);
    if (rc == IB_EEXIST) {
        ib_cfg_log_warning(cp, "Not overwriting existing rule");
        return IB_OK;
    }
    else if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error registering rule: %s",
                         ib_status_to_string(rc));
        return rc;
    }

    /* Disable the entire chain if this rule is invalid */
    if ( (rule->flags & IB_RULE_FLAG_VALID) == 0) {
        rc = ib_rule_chain_invalidate(cp->ib, cp->cur_ctx, rule);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "Error invalidating rule chain: %s",
                             ib_status_to_string(rc));
            return rc;
        }
    }

    ib_cfg_log_debug(cp,
                     "Registered external rule \"%s\" for "
                     "phase \"%s\" context \"%s\"",
                     ib_rule_id(rule),
                     ib_rule_phase_name(rule->meta.phase),
                     ib_context_full_get(cp->cur_ctx));

    /* Done */
    return IB_OK;
}

/**
 * @brief Parse a Rule directive.
 * @details Register a Rule directive to the engine.
 * @param[in,out] cp Configuration parser that contains the engine being
 *                configured.
 * @param[in] name The directive name.
 * @param[in] vars The list of variables passed to @a name.
 * @param[in] cbdata User data. Unused.
 */
static ib_status_t parse_rule_params(ib_cfgparser_t *cp,
                                     const char *name,
                                     const ib_list_t *vars,
                                     void *cbdata)
{
    ib_status_t rc;
    const ib_list_node_t *node;
    const char *nodestr;
    const char *operator;
    const char *operand;
    ib_rule_t *rule = NULL;
    int targets = 0;

    if (cbdata != NULL) {
            }

    /* Allocate a rule */
    rc = ib_rule_create(cp->ib, cp->cur_ctx,
                        cp->cur_file, cp->cur_lineno,
                        false, &rule);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to allocate rule: %s",
                         ib_status_to_string(rc));
        goto cleanup;
    }

    /* Loop through the targets, stop when we encounter an operator */
    IB_LIST_LOOP_CONST(vars, node) {
        if (node->data == NULL) {
            ib_cfg_log_error(cp, "Found invalid rule target");
            rc = IB_EINVAL;
            goto cleanup;
        }
        nodestr = (const char *)node->data;
        if ( (*nodestr == '@') ||
             ((*nodestr != '\0') && (*(nodestr+1) == '@')) )
        {
            break;
        }
        rc = parse_target(cp, rule, nodestr);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp,
                             "Error parsing rule target \"%s\": %s",
                             nodestr, ib_status_to_string(rc));
            goto cleanup;
        }
        ++targets;
    }

    /* No targets??? */
    if (targets == 0) {
        ib_cfg_log_error(cp, "No rule targets found");
        rc = IB_EINVAL;
        goto cleanup;
    }

    /* Verify that we have an operator and operand */
    if ( (node == NULL) || (node-> data == NULL) ) {
        ib_cfg_log_error(cp, "No rule operator found");
        rc = IB_EINVAL;
        goto cleanup;
    }
    operator = (const char *)node->data;
    node = ib_list_node_next_const(node);
    if ( (node == NULL) || (node-> data == NULL) ) {
        ib_cfg_log_error(cp, "No rule operand found");
        rc = IB_EINVAL;
        goto cleanup;
    }
    operand = (const char *)node->data;

    /* Parse the operator */
    rc = parse_operator(cp, rule, operator, operand);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error parsing rule operator \"%s\": %s",
                         operator, ib_status_to_string(rc));
        goto cleanup;
    }

    /* Parse all of the modifiers */
    while( (node = ib_list_node_next_const(node)) != NULL) {
        if (node->data == NULL) {
            ib_cfg_log_error(cp, "Found invalid rule modifier");
            rc = IB_EINVAL;
            goto cleanup;
        }
        nodestr = (const char *)node->data;
        rc = parse_modifier(cp, rule, nodestr);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "Error parsing rule modifier \"%s\": %s",
                             nodestr, ib_status_to_string(rc));
            goto cleanup;
        }
    }

    /* Check the rule modifiers. */
    rc = check_rule_modifiers(cp, rule);
    if (rc != IB_OK) {
        return rc;
    }

    /* Finally, register the rule */
    rc = ib_rule_register(cp->ib, cp->cur_ctx, rule);
    if (rc == IB_EEXIST) {
        ib_cfg_log_warning(cp, "Not overwriting existing rule");
        rc = IB_OK;
    }
    else if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error registering rule: %s",
                         ib_status_to_string(rc));
        goto cleanup;
    }

    /* Disable the entire chain if this rule is invalid */
cleanup:
    if ((rule != NULL) && ((rule->flags & IB_RULE_FLAG_VALID) == 0)) {
        ib_status_t irc = ib_rule_chain_invalidate(cp->ib, cp->cur_ctx, rule);
        if (irc != IB_OK) {
            ib_cfg_log_error(cp, "Error invalidating rule chain: %s",
                             ib_status_to_string(irc));
            return rc;
        }
        else {
            const char *chain = \
                rule->meta.chain_id == NULL ? "UNKNOWN" : rule->meta.chain_id;
            ib_cfg_log_debug2(cp,
                              "Invalidated all rules in chain \"%s\"",
                              chain);
        }
    }

    /* Done */
    return rc;
}

/**
 * @brief Parse a StreamInspect directive.
 * @details Register the StreamInspect directive to the engine.
 *
 * @param[in,out] cp Configuration parser that contains the engine being
 *                configured.
 * @param[in] name The directive name.
 * @param[in] vars The list of variables passed to @a name.
 * @param[in] cbdata User data. Unused.
 */
static ib_status_t parse_streaminspect_params(ib_cfgparser_t *cp,
                                              const char *name,
                                              const ib_list_t *vars,
                                              void *cbdata)
{
    ib_status_t rc;
    const ib_list_node_t *node;
    ib_rule_phase_num_t phase = PHASE_INVALID;
    const char *str;
    const char *operator;
    const char *operand;
    ib_rule_t *rule;

    if (cbdata != NULL) {
            }

    /* Get the phase string */
    node = ib_list_first_const(vars);
    if ( (node == NULL) || (node->data == NULL) ) {
        ib_cfg_log_error(cp, "No stream for StreamInspect");
        return IB_EINVAL;
    }
    str = node->data;

    /* Lookup the phase name */
    phase = ib_rule_lookup_phase(str, true);
    if (phase == PHASE_INVALID) {
        ib_cfg_log_error(cp, "Invalid phase: %s", str);
        return IB_EINVAL;
    }

    /* Get the operator string */
    node = ib_list_node_next_const(node);
    if ( (node == NULL) || (node->data == NULL) ) {
        ib_cfg_log_error(cp, "No operator for rule");
        return IB_EINVAL;
    }
    operator = (const char *)node->data;

    /* Allocate a rule */
    rc = ib_rule_create(cp->ib, cp->cur_ctx,
                        cp->cur_file, cp->cur_lineno,
                        true, &rule);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to create rule: %s",
                         ib_status_to_string(rc));
        return rc;
    }
    ib_flags_set(rule->flags, IB_RULE_FLAG_NO_TGT);

    /* Set the rule's stream */
    rc = ib_rule_set_phase(cp->ib, rule, phase);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error setting rule phase: %s",
                         ib_status_to_string(rc));
        return rc;
    }

    /* Verify that we have an operand */
    node = ib_list_node_next_const(node);
    if ( (node == NULL) || (node-> data == NULL) ) {
        ib_cfg_log_error(cp, "No rule operand found");
        return rc;
    }
    operand = (const char *)node->data;

    /* Parse the operator */
    rc = parse_operator(cp, rule, operator, operand);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Error parsing rule operator \"%s\": %s",
                         operator, ib_status_to_string(rc));
        return rc;
    }

    /* Parse all of the modifiers */
    while( (node = ib_list_node_next_const(node)) != NULL) {
        rc = parse_modifier(cp, rule, node->data);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp,
                             "Error parsing stream rule modifier \"%s\": %s",
                             (const char *)node->data,
                             ib_status_to_string(rc));
            return rc;
        }
    }

    /* Finally, register the rule */
    rc = ib_rule_register(cp->ib, cp->cur_ctx, rule);
    if (rc == IB_EEXIST) {
        ib_cfg_log_warning(cp, "Not overwriting existing rule");
        return IB_OK;
    }
    else if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error registering rule: %s",
                         ib_status_to_string(rc));
        return rc;
    }

    /* Done */
    return IB_OK;
}

/**
 * @brief Parse a RuleEnable directive.
 * @details Handle the RuleEnable directive to the engine.
 *
 * @param[in,out] cp Configuration parser that contains the engine being
 *                configured.
 * @param[in] name The directive name.
 * @param[in] vars The list of variables passed to @c name.
 * @param[in] cbdata User data. Unused.
 */
static ib_status_t parse_ruleenable_params(ib_cfgparser_t *cp,
                                           const char *name,
                                           const ib_list_t *vars,
                                           void *cbdata)
{
    const ib_list_node_t *node;
    ib_status_t rc = IB_OK;

    if (cbdata != NULL) {
            }

    /* Loop through all of the parameters in the list */
    IB_LIST_LOOP_CONST(vars, node) {
        const char *param = (const char *)node->data;

        if (strcasecmp(param, "all") == 0) {
            rc = ib_rule_enable_all(cp->ib, cp->cur_ctx,
                                    cp->cur_file, cp->cur_lineno);
        }
        else if (strncasecmp(param, "id:", 3) == 0) {
            const char *id = param + 3;
            rc = ib_rule_enable_id(cp->ib, cp->cur_ctx,
                                   cp->cur_file, cp->cur_lineno,
                                   id);
        }
        else if (strncasecmp(param, "tag:", 4) == 0) {
            const char *tag = param + 4;
            rc = ib_rule_enable_tag(cp->ib, cp->cur_ctx,
                                    cp->cur_file, cp->cur_lineno,
                                    tag);
        }
        else {
            ib_cfg_log_error(cp, "Invalid %s parameter \"%s\"", name, param);
            rc = IB_EINVAL;
            continue;
        }
    }

    /* Done */
    return rc;
}

/**
 * @brief Parse a RuleDisable directive.
 * @details Handle the RuleDisable directive to the engine.
 *
 * @param[in,out] cp Configuration parser that contains the engine being
 *                configured.
 * @param[in] name The directive name.
 * @param[in] vars The list of variables passed to @c name.
 * @param[in] cbdata User data. Unused.
 */
static ib_status_t parse_ruledisable_params(ib_cfgparser_t *cp,
                                            const char *name,
                                            const ib_list_t *vars,
                                            void *cbdata)
{
    const ib_list_node_t *node;
    ib_status_t rc = IB_OK;

    if (cbdata != NULL) {
            }

    /* Loop through all of the parameters in the list */
    IB_LIST_LOOP_CONST(vars, node) {
        const char *param = (const char *)node->data;

        if (strcasecmp(param, "all") == 0) {
            rc = ib_rule_disable_all(cp->ib, cp->cur_ctx,
                                     cp->cur_file, cp->cur_lineno);
        }
        else if (strncasecmp(param, "id:", 3) == 0) {
            const char *id = param + 3;
            rc = ib_rule_disable_id(cp->ib, cp->cur_ctx,
                                    cp->cur_file, cp->cur_lineno,
                                    id);
        }
        else if (strncasecmp(param, "tag:", 4) == 0) {
            const char *tag = param + 4;
            rc = ib_rule_disable_tag(cp->ib, cp->cur_ctx,
                                     cp->cur_file, cp->cur_lineno,
                                     tag);
        }
        else {
            ib_cfg_log_error(cp, "Invalid %s parameter \"%s\"", name, param);
            rc = IB_EINVAL;
            continue;
        }
    }

    /* Done */
    return rc;
}

/**
 * @brief Parse a RuleMarker directive.
 * @details Register a RuleMarker directive to the engine.
 * @param[in,out] cp Configuration parser that contains the engine being
 *                configured.
 * @param[in] name The directive name.
 * @param[in] vars The list of variables passed to @a name.
 * @param[in] cbdata User data. Unused.
 */
static ib_status_t parse_rulemarker_params(ib_cfgparser_t *cp,
                                           const char *name,
                                           const ib_list_t *vars,
                                           void *cbdata)
{
    ib_status_t rc;
    const ib_list_node_t *node;
    ib_rule_t *rule = NULL;

    if (cbdata != NULL) {
            }

    /* Allocate a rule */
    rc = ib_rule_create(cp->ib, cp->cur_ctx,
                        cp->cur_file, cp->cur_lineno,
                        false, &rule);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to allocate rule: %s",
                         ib_status_to_string(rc));
        return rc;
    }
    ib_flags_set(rule->flags, IB_RULE_FLAG_ACTION);

    /* Force the operator to one that will not execute (negated nop). */
    rc = parse_operator(cp, rule, "!@nop", NULL);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Error parsing rule operator \"nop\": %s",
                         ib_status_to_string(rc));
        return rc;
    }

    /* Parse all of the modifiers, only allowing id and phase. */
    IB_LIST_LOOP_CONST(vars, node) {
        const char *param = (const char *)node->data;

        if (   (strncasecmp(param, "id:", 3) == 0)
            || (strncasecmp(param, "phase:", 6) == 0))
        {
            rc = parse_modifier(cp, rule, node->data);
            if (rc != IB_OK) {
                ib_cfg_log_error(cp,
                                 "Error parsing %s modifier \"%s\": %s",
                                 name,
                                 (const char *)node->data,
                                 ib_status_to_string(rc));
                return rc;
            }
        }
        else {
            ib_cfg_log_error(cp, "Invalid %s parameter \"%s\"", name, param);
            return IB_EINVAL;
        }
    }

    /* Force a zero revision so it can always be overridden. */
    rc = parse_modifier(cp, rule, "rev:0");
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Error parsing %s modifier \"rev:0\": %s",
                         name,
                         ib_status_to_string(rc));
        return rc;
    }

    /* Check the rule modifiers. */
    rc = check_rule_modifiers(cp, rule);
    if (rc != IB_OK) {
        return rc;
    }

    /* Finally, register the rule. */
    rc = ib_rule_register(cp->ib, cp->cur_ctx, rule);
    if (rc == IB_EEXIST) {
        ib_cfg_log_notice(cp, "Not overwriting existing rule");
        rc = IB_OK;
    }
    else if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error registering rule marker: %s",
                         ib_status_to_string(rc));
    }

    return rc;
}

/**
 * @brief Parse an Action directive.
 * @details Register an Action directive to the engine.
 * @param[in,out] cp Configuration parser that contains the engine being
 *                configured.
 * @param[in] name The directive name.
 * @param[in] vars The list of variables passed to @a name.
 * @param[in] cbdata User data. Unused.
 */
static ib_status_t parse_action_params(ib_cfgparser_t *cp,
                                       const char *name,
                                       const ib_list_t *vars,
                                       void *cbdata)
{
    ib_status_t rc;
    const ib_list_node_t *node;
    ib_rule_t *rule = NULL;

    if (cbdata != NULL) {
            }

    /* Allocate a rule */
    rc = ib_rule_create(cp->ib, cp->cur_ctx,
                        cp->cur_file, cp->cur_lineno,
                        false, &rule);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to allocate rule: %s",
                         ib_status_to_string(rc));
        goto cleanup;
    }
    ib_flags_set(rule->flags, IB_RULE_FLAG_ACTION);

    /* Parse the operator */
    rc = parse_operator(cp, rule, "@nop", NULL);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Error parsing rule operator \"nop\": %s",
                         ib_status_to_string(rc));
        goto cleanup;
    }

    /* Parse all of the modifiers */
    IB_LIST_LOOP_CONST(vars, node) {
        rc = parse_modifier(cp, rule, node->data);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp,
                             "Error parsing action modifier \"%s\": %s",
                             (const char *)node->data,
                             ib_status_to_string(rc));
            goto cleanup;
        }
    }

    /* Check the rule modifiers. */
    rc = check_rule_modifiers(cp, rule);
    if (rc != IB_OK) {
        return rc;
    }

    /* Finally, register the rule */
    rc = ib_rule_register(cp->ib, cp->cur_ctx, rule);
    if (rc == IB_EEXIST) {
        ib_cfg_log_warning(cp, "Not overwriting existing rule");
        rc = IB_OK;
    }
    else if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error registering rule: %s",
                         ib_status_to_string(rc));
        goto cleanup;
    }

    /* Disable the entire chain if this rule is invalid */
cleanup:
    if ((rule != NULL) && ((rule->flags & IB_RULE_FLAG_VALID) == 0)) {
        ib_status_t irc = ib_rule_chain_invalidate(cp->ib, cp->cur_ctx, rule);
        if (irc != IB_OK) {
            ib_cfg_log_error(cp, "Error invalidating rule chain: %s",
                             ib_status_to_string(irc));
            return rc;
        }
        else {
            const char *chain = \
                rule->meta.chain_id == NULL ? "UNKNOWN" : rule->meta.chain_id;
            ib_cfg_log_debug2(cp,
                              "Invalidated all rules in chain \"%s\"",
                              chain);
        }
    }

    /* Done */
    return rc;
}

static IB_DIRMAP_INIT_STRUCTURE(rules_directive_map) = {

    /* Give the config parser a callback for the Rule and RuleExt directive */
    IB_DIRMAP_INIT_LIST(
        "Rule",
        parse_rule_params,
        NULL
    ),

    IB_DIRMAP_INIT_LIST(
        "RuleExt",
        parse_ruleext_params,
        NULL
    ),

    IB_DIRMAP_INIT_LIST(
        "RuleMarker",
        parse_rulemarker_params,
        NULL
    ),

    IB_DIRMAP_INIT_LIST(
        "StreamInspect",
        parse_streaminspect_params,
        NULL
    ),

    IB_DIRMAP_INIT_LIST(
        "RuleEnable",
        parse_ruleenable_params,
        NULL
    ),

    IB_DIRMAP_INIT_LIST(
        "RuleDisable",
        parse_ruledisable_params,
        NULL
    ),

    IB_DIRMAP_INIT_LIST(
        "Action",
        parse_action_params,
        NULL
    ),

    /* signal the end of the list */
    IB_DIRMAP_INIT_LAST
};

/* Initialize the module structure. */
IB_MODULE_INIT(
    IB_MODULE_HEADER_DEFAULTS,           /* Default metadata */
    MODULE_NAME_STR,                     /* Module name */
    IB_MODULE_CONFIG_NULL,               /* Global config data */
    NULL,                                /* Configuration field map */
    rules_directive_map,                 /* Config directive map */
    NULL,                                /* Initialize function */
    NULL,                                /* Callback data */
    NULL,                                /* Finish function */
    NULL,                                /* Callback data */
    NULL,                                /* Context open function */
    NULL,                                /* Callback data */
    NULL,                                /* Context close function */
    NULL,                                /* Callback data */
    NULL,                                /* Context destroy function */
    NULL                                 /* Callback data */
);
