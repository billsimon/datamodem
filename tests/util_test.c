/* Unit tests for the small shared pieces in util.c: caller ID parsing. */
#include "datamodem/util.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check_party(const char *in, const char *want_name, const char *want_user)
{
    char name[64], user[64];

    dm_parse_sip_party(in, name, sizeof(name), user, sizeof(user));
    if (strcmp(name, want_name) != 0 || strcmp(user, want_user) != 0)
    {
        printf("FAIL %-55s name=[%s] want [%s], user=[%s] want [%s]\n", in ? in : "(null)", name,
               want_name, user, want_user);
        failures++;
    }
}

int main(void)
{
    /* quoted display names, with escapes */
    check_party("\"Alice Smith\" <sip:+15551234@trunk.example.com;user=phone>", "Alice Smith", "+15551234");
    check_party("\"say \\\"hi\\\"\" <sip:100@pbx>", "say \"hi\"", "100");
    check_party("  \"\" <sip:5550100@host>", "", "5550100");
    /* unquoted display names */
    check_party("Bob <sips:bob@example.com>", "Bob", "bob");
    check_party("Carol  Jones   <sip:200@x>", "Carol  Jones", "200");
    /* bare URIs and no user part */
    check_party("<sip:call@127.0.0.1>", "", "call");
    check_party("sip:5551234@host", "", "5551234");
    check_party("sip:host.example.com", "", "");
    check_party("<sip:host.example.com;transport=tcp>", "", "");
    check_party("sip:host;user=phone", "", "");
    /* tel: URIs have no host */
    check_party("tel:+15551234;phone-context=example.com", "", "+15551234");
    check_party("<tel:911>", "", "911");
    check_party("tel:+15550000", "", "+15550000");
    /* percent-encoding, and control characters dropped */
    check_party("<sip:%2B1555%31@host>", "", "+15551");
    check_party("\"Evil\x1b[2J\" <sip:6%0A6@host>", "Evil[2J", "66");
    /* anonymous, garbage, and nothing at all */
    check_party("\"Anonymous\" <sip:anonymous@anonymous.invalid>", "Anonymous", "anonymous");
    check_party("not a uri", "", "");
    check_party("", "", "");
    check_party(NULL, "", "");

    /* truncation to the space given, always terminated */
    {
        char name[4], user[4];
        dm_parse_sip_party("\"Longname\" <sip:123456789@h>", name, sizeof(name), user, sizeof(user));
        if (strcmp(name, "Lon") != 0 || strcmp(user, "123") != 0)
        {
            printf("FAIL truncation: name=[%s] user=[%s]\n", name, user);
            failures++;
        }
    }

    if (failures == 0)
        printf("util: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
