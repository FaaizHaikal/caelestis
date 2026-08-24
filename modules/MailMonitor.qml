import QtQuick
import Quickshell
import Caelestia
import Caelestia.Config
import Caelestia.Services

Scope {
    id: root

    readonly property bool enabled: GlobalConfig.general.mail.enabled
    readonly property string email: GlobalConfig.general.mail.email
    readonly property string clientId: GlobalConfig.general.mail.clientId
    readonly property string clientSecret: GlobalConfig.general.mail.clientSecret

    Component.onCompleted: {
        if (root.enabled && root.email !== "") {
            MailProvider.init(root.email, root.clientId, root.clientSecret);
        }
    }

    Connections {
        target: MailProvider

        function onNewMailReceived(sender: string, subject: string, snippet: string): void {
            const title = subject !== "" ? subject : qsTr("No Subject");
            const match = sender.match(/<([^>]+)>/);
            const emailSender = match ? match[1].trim() : sender.replace(/["']/g, "").trim();
            const icon = Qt.resolvedUrl(`${Quickshell.shellDir}/assets/mail.svg`);

            Quickshell.execDetached(["notify-send", "-a", "New Email", "-i", icon, title, emailSender]);
        }

        function onAuthRequired(): void {
            Toaster.toast(qsTr("Mail Authentication"), qsTr("Please log in to your email account in the browser"), "lock", Toast.Warning);
            MailProvider.startLoginFlow();
        }
    }
}
