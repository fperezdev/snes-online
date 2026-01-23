import UIKit

final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene,
               willConnectTo session: UISceneSession,
               options connectionOptions: UIScene.ConnectionOptions) {
        guard let windowScene = scene as? UIWindowScene else { return }

        let window = UIWindow(windowScene: windowScene)
        let root = IOSConfigViewController()
        let nav = UINavigationController(rootViewController: root)
        window.rootViewController = nav
        window.makeKeyAndVisible()
        self.window = window

        if let url = connectionOptions.urlContexts.first?.url {
            root.applyInviteLink(url)
        }
    }

    func scene(_ scene: UIScene, openURLContexts URLContexts: Set<UIOpenURLContext>) {
        guard let url = URLContexts.first?.url else { return }
        guard let nav = window?.rootViewController as? UINavigationController,
              let root = nav.viewControllers.first as? IOSConfigViewController else {
            return
        }
        root.applyInviteLink(url)
    }
}
