/*
 * bankctl.c - a tiny command-line program to try the banking functions by hand.
 * It is NOT the network server; it just proves the core works.
 *
 * USAGE
 *   bankctl <datadir> init                        create data files + admin/admin123
 *   bankctl <datadir> <user> <password> <command> [args...]
 *
 * COMMANDS (who can run them)
 *   balance                          customer
 *   deposit <amount>                 customer
 *   withdraw <amount>                customer
 *   transfer <to_account> <amount>   customer
 *   history [account_id]             customer (own) / staff (any)
 *   loan-apply <amount>              customer
 *   feedback "<text>"                customer
 *   passwd <new_password>            everyone
 *   add-customer <user> <pass> "<name>" <initial_deposit>     employee
 *   modify-user <user_id> "<new name>"                         employee / admin
 *   loans                            employee (loans assigned to me)
 *   loan-process <loan_id> approve|reject                      employee
 *   account-active <account_id> 0|1  manager
 *   loan-assign <loan_id> <employee_user_id>                   manager
 *   feedback-list                    manager
 *   add-staff <user> <pass> "<name>" employee|manager          admin
 *   set-role <user_id> employee|manager                        admin
 *   hold <seconds>                   stay logged in (to test "one session per user")
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bank.h"

static int64_t num(const char *s) { return strtoll(s, NULL, 10); }

static const char *tx_name(int t)
{
    switch (t) {
    case TX_DEPOSIT:      return "DEPOSIT";
    case TX_WITHDRAW:     return "WITHDRAW";
    case TX_TRANSFER_OUT: return "TRANSFER-OUT";
    case TX_TRANSFER_IN:  return "TRANSFER-IN";
    case TX_LOAN_CREDIT:  return "LOAN-CREDIT";
    }
    return "?";
}

static const char *loan_status(int s)
{
    switch (s) {
    case LOAN_PENDING:  return "PENDING";
    case LOAN_ASSIGNED: return "ASSIGNED";
    case LOAN_APPROVED: return "APPROVED";
    case LOAN_REJECTED: return "REJECTED";
    }
    return "?";
}

/* print the outcome of a call and return the exit code */
static int report(int rc, const char *ok_msg)
{
    if (rc == BANK_OK) { printf("OK: %s\n", ok_msg); return 0; }
    printf("ERROR: %s\n", bank_strerror(rc));
    return 1;
}

int main(int argc, char **argv)
{
    Db db;

    if (argc < 3) { fprintf(stderr, "usage: see comment at top of tools/bankctl.c\n"); return 2; }
    if (db_open(&db, argv[1]) < 0) { perror("db_open"); return 2; }

    /* ---- init ---- */
    if (strcmp(argv[2], "init") == 0) {
        int rc = bank_create_first_admin(&db, "admin", "admin123");
        return report(rc, "data files ready; admin user = admin / admin123");
    }

    /* ---- log in ---- */
    if (argc < 5) { fprintf(stderr, "need: <user> <password> <command>\n"); return 2; }
    User me;
    int session_fd;
    int rc = bank_login(&db, argv[2], argv[3], &me, &session_fd);
    if (rc != BANK_OK) return report(rc, "");

    const char *cmd = argv[4];
    char **a = argv + 5;           /* command arguments */
    int na = argc - 5;
    int result = 0;
    int64_t bal;
    int id;

    if (!strcmp(cmd, "balance")) {
        rc = bank_balance(&db, &me, &bal);
        if (rc == BANK_OK) printf("balance = %lld\n", (long long)bal);
        result = report(rc, "done");
    } else if (!strcmp(cmd, "deposit") && na >= 1) {
        rc = bank_deposit(&db, &me, num(a[0]), &bal);
        if (rc == BANK_OK) printf("new balance = %lld\n", (long long)bal);
        result = report(rc, "deposited");
    } else if (!strcmp(cmd, "withdraw") && na >= 1) {
        rc = bank_withdraw(&db, &me, num(a[0]), &bal);
        if (rc == BANK_OK) printf("new balance = %lld\n", (long long)bal);
        result = report(rc, "withdrawn");
    } else if (!strcmp(cmd, "transfer") && na >= 2) {
        rc = bank_transfer(&db, &me, (int)num(a[0]), num(a[1]), &bal);
        if (rc == BANK_OK) printf("new balance = %lld\n", (long long)bal);
        result = report(rc, "transferred");
    } else if (!strcmp(cmd, "history")) {
        TxnRec lines[100];
        int n = 0;
        int acct = (na >= 1) ? (int)num(a[0]) : me.account_id;
        rc = bank_history(&db, &me, acct, lines, 100, &n);
        for (int i = 0; rc == BANK_OK && i < n; i++)
            printf("#%d  %-13s amount=%lld  other_acct=%d  balance_after=%lld\n",
                   lines[i].id, tx_name(lines[i].type), (long long)lines[i].amount,
                   lines[i].other_account, (long long)lines[i].balance_after);
        result = report(rc, "history shown");
    } else if (!strcmp(cmd, "loan-apply") && na >= 1) {
        rc = bank_apply_loan(&db, &me, num(a[0]), &id);
        if (rc == BANK_OK) printf("loan id = %d\n", id);
        result = report(rc, "loan applied");
    } else if (!strcmp(cmd, "feedback") && na >= 1) {
        result = report(bank_add_feedback(&db, &me, a[0]), "feedback saved");
    } else if (!strcmp(cmd, "passwd") && na >= 1) {
        result = report(bank_change_password(&db, &me, a[0]), "password changed");
    } else if (!strcmp(cmd, "add-customer") && na >= 4) {
        rc = bank_add_customer(&db, &me, a[0], a[1], a[2], num(a[3]), &id);
        if (rc == BANK_OK) {
            User nu;
            bank_get_user(&db, &me, id, &nu);
            printf("new user id = %d, account id = %d\n", id, nu.account_id);
        }
        result = report(rc, "customer added");
    } else if (!strcmp(cmd, "user-info") && na >= 1) {
        User u;
        rc = bank_get_user(&db, &me, (int)num(a[0]), &u);
        if (rc == BANK_OK)
            printf("user %d: login=%s name=\"%s\" role=%d active=%d account=%d\n",
                   u.id, u.username, u.name, u.role, u.active, u.account_id);
        result = report(rc, "user shown");
    } else if (!strcmp(cmd, "modify-user") && na >= 2) {
        result = report(bank_modify_user(&db, &me, (int)num(a[0]), a[1]), "user modified");
    } else if (!strcmp(cmd, "loans")) {
        Loan loans[50];
        int n = 0;
        rc = bank_list_assigned_loans(&db, &me, loans, 50, &n);
        for (int i = 0; rc == BANK_OK && i < n; i++)
            printf("loan #%d  amount=%lld  customer=%d  status=%s\n", loans[i].id,
                   (long long)loans[i].amount, loans[i].customer_id, loan_status(loans[i].status));
        result = report(rc, "loans shown");
    } else if (!strcmp(cmd, "loan-process") && na >= 2) {
        result = report(bank_process_loan(&db, &me, (int)num(a[0]), !strcmp(a[1], "approve")),
                        "loan processed");
    } else if (!strcmp(cmd, "account-active") && na >= 2) {
        result = report(bank_set_account_active(&db, &me, (int)num(a[0]), (int)num(a[1])),
                        "account updated");
    } else if (!strcmp(cmd, "loan-assign") && na >= 2) {
        result = report(bank_assign_loan(&db, &me, (int)num(a[0]), (int)num(a[1])), "loan assigned");
    } else if (!strcmp(cmd, "feedback-list")) {
        Feedback fb[50];
        int n = 0;
        rc = bank_list_feedback(&db, &me, fb, 50, &n);
        for (int i = 0; rc == BANK_OK && i < n; i++)
            printf("from user %d: %s\n", fb[i].user_id, fb[i].text);
        result = report(rc, "feedback shown");
    } else if (!strcmp(cmd, "add-staff") && na >= 4) {
        int role = !strcmp(a[3], "manager") ? ROLE_MANAGER : ROLE_EMPLOYEE;
        rc = bank_add_employee(&db, &me, a[0], a[1], a[2], role, &id);
        if (rc == BANK_OK) printf("new user id = %d\n", id);
        result = report(rc, "staff added");
    } else if (!strcmp(cmd, "set-role") && na >= 2) {
        int role = !strcmp(a[1], "manager") ? ROLE_MANAGER : ROLE_EMPLOYEE;
        result = report(bank_set_role(&db, &me, (int)num(a[0]), role), "role changed");
    } else if (!strcmp(cmd, "hold") && na >= 1) {
        printf("logged in as %s; holding the session for %s seconds...\n", me.username, a[0]);
        fflush(stdout);
        sleep((unsigned)num(a[0]));
        result = report(BANK_OK, "session ended");
    } else {
        fprintf(stderr, "unknown command or missing arguments: %s\n", cmd);
        result = 2;
    }

    session_release(session_fd);   /* logout */
    db_close(&db);
    return result;
}
