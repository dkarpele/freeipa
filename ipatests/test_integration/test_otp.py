#
# Copyright (C) 2019  FreeIPA Contributors see COPYING for license
#
"""OTP token tests
"""
import base64
import logging
import paramiko
import pytest
import re
import time
import textwrap
from urllib.parse import urlparse, parse_qs

from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.twofactor.hotp import HOTP
from cryptography.hazmat.primitives.twofactor.totp import TOTP

from ipatests.test_integration.base import IntegrationTest
from ipaplatform.paths import paths
from ipatests.pytest_ipa.integration import tasks
from ipapython.dn import DN


PASSWORD = "DummyPassword123"
USER = "opttestuser"
ARMOR = "/tmp/armor"
logger = logging.getLogger(__name__)


def add_otptoken(host, owner, *, otptype="hotp", digits=6, algo="sha1"):
    args = [
        "ipa",
        "otptoken-add",
        "--owner",
        owner,
        "--type",
        otptype,
        "--digits",
        str(digits),
        "--algo",
        algo,
        "--no-qrcode",
    ]
    result = host.run_command(args)
    otpuid = re.search(
        r"Unique ID:\s*([a-z0-9-]*)\s+", result.stdout_text
    ).group(1)
    otpuristr = re.search(r"URI:\s*(.*)\s+", result.stdout_text).group(1)
    otpuri = urlparse(otpuristr)
    assert otpuri.netloc == otptype

    query = parse_qs(otpuri.query)
    assert query["algorithm"][0] == algo.upper()
    assert query["digits"][0] == str(digits)
    key = base64.b32decode(query["secret"][0])
    assert len(key) == 35

    hashcls = getattr(hashes, algo.upper())
    if otptype == "hotp":
        return otpuid, HOTP(key, digits, hashcls(), default_backend())
    else:
        period = int(query["period"][0])
        return otpuid, TOTP(key, digits, hashcls(), period, default_backend())


def del_otptoken(host, otpuid):
    tasks.kinit_admin(host)
    host.run_command(["ipa", "otptoken-del", otpuid])


def kinit_otp(host, user, *, password, otp, success=True):
    tasks.kdestroy_all(host)
    # create armor for FAST
    host.run_command(["kinit", "-n", "-c", ARMOR])
    host.run_command(
        ["kinit", "-T", ARMOR, user],
        stdin_text=f"{password}{otp}\n",
        ok_returncode=0 if success else 1,
    )


def ssh_2f(hostname, username, answers_dict, port=22):
    """
    :param hostname: hostname
    :param username: username
    :param answers_dict: dictionary of options with prompt_message and value.
    :param port: port for ssh
    """
    # Handler for server questions
    def answer_handler(title, instructions, prompt_list):
        resp = []
        if title:
            print(title.strip())
        if instructions:
            print(instructions.strip())
        for prmpt in prompt_list:
            prmpt_str = prmpt[0].strip()
            resp.append(answers_dict[prmpt_str])
            logger.info("Prompt is: '%s'", prmpt_str)
            logger.info(
                "Answer to ssh prompt is: '%s'", answers_dict[prmpt_str])
        return resp
    trans = paramiko.Transport((hostname, port))
    trans.connect()
    trans.auth_interactive(username, answer_handler)


def set_sssd_conf(host, add_contents):
    contents = host.get_file_contents(paths.SSSD_CONF, encoding="utf-8")
    file_contents = contents + add_contents
    host.put_file_contents(paths.SSSD_CONF, file_contents)
    tasks.clear_sssd_cache(host)


class TestOTPToken(IntegrationTest):
    """Tests for member manager feature for groups and hostgroups
    """

    topology = "line"

    @classmethod
    def install(cls, mh):
        super(TestOTPToken, cls).install(mh)
        master = cls.master

        tasks.kinit_admin(master)
        # create service with OTP auth indicator
        cls.service_name = f"otponly/{master.hostname}"
        master.run_command(
            ["ipa", "service-add", cls.service_name, "--auth-ind=otp"]
        )
        # service needs a keytab before user can acquire a ticket for it
        keytab = "/tmp/otponly.keytab"
        master.run_command(
            ["ipa-getkeytab", "-p", cls.service_name, "-k", keytab]
        )
        master.run_command(["rm", "-f", keytab])

        tasks.create_active_user(master, USER, PASSWORD)
        tasks.kinit_admin(master)
        master.run_command(["ipa", "user-mod", USER, "--user-auth-type=otp"])

    @classmethod
    def uninstall(cls, mh):
        cls.master.run_command(["rm", "-f", ARMOR])
        super(TestOTPToken, cls).uninstall(mh)

    def test_otp_auth_ind(self):
        tasks.kinit_admin(self.master)
        result = self.master.run_command(
            ["kvno", self.service_name], ok_returncode=1
        )
        assert "KDC policy rejects request" in result.stderr_text

    def test_hopt(self):
        master = self.master

        tasks.kinit_admin(self.master)
        otpuid, hotp = add_otptoken(master, USER, otptype="hotp")
        master.run_command(["ipa", "otptoken-show", otpuid])
        # normal password login fails
        master.run_command(
            ["kinit", USER], stdin_text=f"{PASSWORD}\n", ok_returncode=1
        )
        # OTP login works
        otpvalue = hotp.generate(0).decode("ascii")
        kinit_otp(master, USER, password=PASSWORD, otp=otpvalue)
        # repeating OTP fails
        kinit_otp(
            master, USER, password=PASSWORD, otp=otpvalue, success=False
        )
        # skipping an OTP is ok
        otpvalue = hotp.generate(2).decode("ascii")
        kinit_otp(master, USER, password=PASSWORD, otp=otpvalue)
        # TGT with OTP auth indicator can get a ticket for OTP-only service
        master.run_command(["kvno", self.service_name])
        result = master.run_command(["klist"])
        assert self.service_name in result.stdout_text

        del_otptoken(master, otpuid)

    def test_totp(self):
        master = self.master

        tasks.kinit_admin(self.master)
        otpuid, totp = add_otptoken(master, USER, otptype="totp")

        otpvalue = totp.generate(int(time.time())).decode("ascii")
        kinit_otp(master, USER, password=PASSWORD, otp=otpvalue)
        # TGT with OTP auth indicator can get a ticket for OTP-only service
        master.run_command(["kvno", self.service_name])
        result = master.run_command(["klist"])
        assert self.service_name in result.stdout_text

        del_otptoken(master, otpuid)

    def test_otptoken_sync(self):
        master = self.master

        tasks.kinit_admin(self.master)
        otpuid, hotp = add_otptoken(master, USER, otptype="hotp")

        otp1 = hotp.generate(10).decode("ascii")
        otp2 = hotp.generate(11).decode("ascii")

        master.run_command(
            ["ipa", "otptoken-sync", "--user", USER],
            stdin_text=f"{PASSWORD}\n{otp1}\n{otp2}\n",
        )
        otpvalue = hotp.generate(12).decode("ascii")
        kinit_otp(master, USER, password=PASSWORD, otp=otpvalue)

        otp1 = hotp.generate(20).decode("ascii")
        otp2 = hotp.generate(21).decode("ascii")

        master.run_command(
            ["ipa", "otptoken-sync", otpuid, "--user", USER],
            stdin_text=f"{PASSWORD}\n{otp1}\n{otp2}\n",
        )
        otpvalue = hotp.generate(22).decode("ascii")
        kinit_otp(master, USER, password=PASSWORD, otp=otpvalue)

        del_otptoken(master, otpuid)

    def test_2fa_enable_single_prompt(self):
        """Test ssh with 2FA when single prompt is enabled.

        Test for : https://pagure.io/SSSD/sssd/issue/3264

        When [prompting/2fa/sshd] with single_prompt = True is set
        then during ssh it should be prompted with given message
        for first and second factor at once.
        """

        master = self.master
        USER1 = 'sshuser1'
        sssd_conf_backup = tasks.FileBackup(master, paths.SSSD_CONF)
        first_prompt = 'Please enter password + OTP token value:'
        add_contents = textwrap.dedent('''
            [prompting/2fa/sshd]
            single_prompt = True
            first_prompt = {0}
            ''').format(first_prompt)
        set_sssd_conf(master, add_contents)
        tasks.create_active_user(master, USER1, PASSWORD)
        tasks.kinit_admin(master)
        master.run_command(['ipa', 'user-mod', USER1, '--user-auth-type=otp'])
        try:
            otpuid, totp = add_otptoken(master, USER1, otptype='totp')
            master.run_command(['ipa', 'otptoken-show', otpuid])
            otpvalue = totp.generate(int(time.time())).decode('ascii')
            password = '{0}{1}'.format(PASSWORD, otpvalue)
            tasks.run_ssh_cmd(
                to_host=self.master.external_hostname, username=USER1,
                auth_method="password", password=password
            )
            # check if user listed in output
            cmd = self.master.run_command(['semanage', 'login', '-l'])
            assert USER1 in cmd.stdout_text
        finally:
            master.run_command(['ipa', 'user-del', USER1])
            self.master.run_command(['semanage', 'login', '-D'])
            sssd_conf_backup.restore()

    def test_2fa_disable_single_prompt(self):
        """Test ssh with 2FA when single prompt is disabled.

        Test for : https://pagure.io/SSSD/sssd/issue/3264

        When [prompting/2fa/sshd] with single_prompt = False is set
        then during ssh it should be prompted with given message
        for first factor and then for second factor.

        This requires paramiko until the 2-prompt sshpass RFE is
        fulfilled: https://sourceforge.net/p/sshpass/feature-requests/5/
        """
        if self.master.is_fips_mode:
            pytest.skip("paramiko is not compatible with FIPS mode")

        master = self.master
        USER2 = 'sshuser2'
        sssd_conf_backup = tasks.FileBackup(master, paths.SSSD_CONF)
        first_prompt = 'Enter first factor:'
        second_prompt = 'Enter second factor:'
        add_contents = textwrap.dedent('''
            [prompting/2fa/sshd]
            single_prompt = False
            first_prompt = {0}
            second_prompt = {1}
            ''').format(first_prompt, second_prompt)
        set_sssd_conf(master, add_contents)
        tasks.create_active_user(master, USER2, PASSWORD)
        tasks.kinit_admin(master)
        master.run_command(['ipa', 'user-mod', USER2, '--user-auth-type=otp'])
        try:
            otpuid, totp = add_otptoken(master, USER2, otptype='totp')
            master.run_command(['ipa', 'otptoken-show', otpuid])
            otpvalue = totp.generate(int(time.time())).decode('ascii')
            answers = {
                first_prompt: PASSWORD,
                second_prompt: otpvalue
            }
            ssh_2f(master.hostname, USER2, answers)
            # check if user listed in output
            cmd = self.master.run_command(['semanage', 'login', '-l'])
            assert USER2 in cmd.stdout_text
        finally:
            master.run_command(['ipa', 'user-del', USER2])
            self.master.run_command(['semanage', 'login', '-D'])
            sssd_conf_backup.restore()

    @pytest.fixture
    def setup_otp_nsslapd(self):
        check_services = self.master.run_command(
            ['systemctl', 'list-units', '--state=failed']
        )
        assert "ipa-otpd" not in check_services.stdout_text
        # Be sure no services are running and failed units
        self.master.run_command(['killall', 'ipa-otpd'], raiseonerr=False)
        # setting nsslapd-idletimeout
        new_limit = 30
        conn = self.master.ldap_connect()
        dn = DN(('cn', 'config'))
        entry = conn.get_entry(dn)
        orig_limit = entry.single_value.get('nsslapd-idletimeout')
        ldap_query = textwrap.dedent("""
            dn: cn=config
            changetype: modify
            replace: nsslapd-idletimeout
            nsslapd-idletimeout: {limit}
        """)
        tasks.ldapmodify_dm(self.master, ldap_query.format(limit=new_limit))
        yield
        # cleanup
        tasks.ldapmodify_dm(self.master, ldap_query.format(limit=orig_limit))

    def test_check_otpd_after_idle_timeout(self, setup_otp_nsslapd):
        """Test for OTP when the LDAP connection timed out.

        Test for : https://pagure.io/freeipa/issue/6587

        ipa-otpd was exiting with failure when LDAP connection timed out.
        Test to verify that when the nsslapd-idletimeout is exceeded (30s idle,
        60s sleep) then the ipa-otpd process should exit without error.
        """
        since = time.strftime('%Y-%m-%d %H:%M:%S')
        tasks.kinit_admin(self.master)
        otpuid, totp = add_otptoken(self.master, USER, otptype="totp")
        try:
            # kinit with OTP auth
            otpvalue = totp.generate(int(time.time())).decode("ascii")
            kinit_otp(self.master, USER, password=PASSWORD, otp=otpvalue)
            time.sleep(60)
            # ldapsearch will wake up slapd and force walking through
            # the connection list, in order to spot the idle connections
            tasks.ldapsearch_dm(self.master, "", ldap_args=[], scope="base")

            def test_cb(cmd_jornalctl):
                # check if LDAP connection is timed out
                expected_msg = "Can't contact LDAP server"
                return expected_msg in cmd_jornalctl

            # ipa-otpd don't flush its logs to syslog immediately
            cmd = ['journalctl', '--since={}'.format(since)]
            tasks.run_repeatedly(
                self.master, command=cmd, test=test_cb, timeout=90)
            failed_services = self.master.run_command(
                ['systemctl', 'list-units', '--state=failed']
            )
            assert "ipa-otpd" not in failed_services.stdout_text
        finally:
            del_otptoken(self.master, otpuid)
